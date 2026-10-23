//! Import the textual subset of Linux candump logs.
//! Supported: (timestamp) can0 123#1122, 123#R8, 123##1AABB,
//! and can0 123 [2] 11 22. Unknown/bad data is an error, never silently lost.

use std::fmt;
use std::io::{self, BufRead};

use crate::{CanFrame, CanId, FdFlags, FrameError, FrameKind};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CandumpError {
    Missing(&'static str),
    InvalidTimestamp(String),
    InvalidId(String),
    InvalidPayload(String),
    InvalidFlags(String),
    InvalidFrame(FrameError),
}

impl fmt::Display for CandumpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Missing(what) => write!(f, "missing {what}"),
            Self::InvalidTimestamp(value) => write!(f, "invalid timestamp: {value}"),
            Self::InvalidId(value) => write!(f, "invalid CAN ID: {value}"),
            Self::InvalidPayload(value) => write!(f, "invalid CAN payload: {value}"),
            Self::InvalidFlags(value) => write!(f, "invalid CAN FD flags: {value}"),
            Self::InvalidFrame(source) => source.fmt(f),
        }
    }
}

impl std::error::Error for CandumpError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::InvalidFrame(err) => Some(err),
            _ => None,
        }
    }
}

impl From<FrameError> for CandumpError {
    fn from(value: FrameError) -> Self {
        Self::InvalidFrame(value)
    }
}

#[derive(Debug)]
pub enum TraceReadError {
    Io(io::Error),
    Line { number: usize, source: CandumpError },
}

impl fmt::Display for TraceReadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(err) => err.fmt(f),
            Self::Line { number, source } => write!(f, "candump line {number}: {source}"),
        }
    }
}

impl std::error::Error for TraceReadError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(err) => Some(err),
            Self::Line { source, .. } => Some(source),
        }
    }
}

fn parse_timestamp(token: &str) -> Result<u64, CandumpError> {
    let (seconds, fraction) = token.split_once('.').unwrap_or((token, ""));
    let sec = seconds.parse::<u64>()
        .map_err(|_| CandumpError::InvalidTimestamp(token.into()))?;
    if fraction.len() > 9 || !fraction.bytes().all(|byte| byte.is_ascii_digit()) {
        return Err(CandumpError::InvalidTimestamp(token.into()));
    }
    let micros = if fraction.is_empty() {
        0
    } else {
        // Preserve six digits of precision; discard nanosecond remainder.
        let take = fraction.len().min(6);
        fraction[..take].parse::<u64>()
            .map_err(|_| CandumpError::InvalidTimestamp(token.into()))?
            * 10_u64.pow((6 - take) as u32)
    };
    sec.checked_mul(1_000_000)
        .and_then(|value| value.checked_add(micros))
        .ok_or_else(|| CandumpError::InvalidTimestamp(token.into()))
}

fn parse_id(raw: &str) -> Result<CanId, CandumpError> {
    if raw.is_empty() || raw.len() > 8 || !raw.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(CandumpError::InvalidId(raw.into()));
    }
    let id = u32::from_str_radix(raw, 16)
        .map_err(|_| CandumpError::InvalidId(raw.into()))?;
    // Width matters: 00000123 is explicitly an extended identifier.
    if raw.len() <= 3 {
        CanId::standard(id as u16).map_err(Into::into)
    } else {
        CanId::extended(id).map_err(Into::into)
    }
}

fn hex_bytes(raw: &str) -> Result<Vec<u8>, CandumpError> {
    if raw.len() % 2 != 0 || !raw.is_ascii() {
        return Err(CandumpError::InvalidPayload(raw.into()));
    }
    raw.as_bytes().chunks_exact(2).map(|chunk| {
        let pair = std::str::from_utf8(chunk).expect("ASCII checked");
        u8::from_str_radix(pair, 16)
            .map_err(|_| CandumpError::InvalidPayload(raw.into()))
    }).collect()
}

/// Parse a single line. Blank lines and comment lines are skipped.
pub fn parse_candump_line(line: &str) -> Result<Option<CanFrame>, CandumpError> {
    let mut rest = line.trim();
    if rest.is_empty() || rest.starts_with('#') {
        return Ok(None);
    }
    let timestamp_us = if rest.starts_with('(') {
        let close = rest.find(')').ok_or(CandumpError::Missing("closing timestamp ')'"))?;
        let ts = parse_timestamp(&rest[1..close])?;
        rest = rest[close + 1..].trim_start();
        Some(ts)
    } else {
        None
    };
    let mut tokens = rest.split_whitespace();
    let interface = tokens.next().ok_or(CandumpError::Missing("CAN interface"))?;
    let frame_token = tokens.next().ok_or(CandumpError::Missing("CAN frame"))?;

    let (id, kind, data, is_fd, flags) = if let Some((id_text, encoded)) = frame_token.split_once('#') {
        let id = parse_id(id_text)?;
        if let Some(fd_data) = encoded.strip_prefix('#') {
            let (flag, bytes) = fd_data.split_at(fd_data.chars().next().map(char::len_utf8).unwrap_or(0));
            if flag.len() != 1 {
                return Err(CandumpError::InvalidFlags(encoded.into()));
            }
            let flag_value = u8::from_str_radix(flag, 16)
                .map_err(|_| CandumpError::InvalidFlags(flag.into()))?;
            if flag_value & !0x3 != 0 {
                return Err(CandumpError::InvalidFlags(flag.into()));
            }
            (
                id,
                FrameKind::Data,
                hex_bytes(bytes)?,
                true,
                FdFlags {
                    bitrate_switch: flag_value & 1 != 0,
                    error_state_indicator: flag_value & 2 != 0,
                },
            )
        } else if let Some(dlc_text) = encoded.strip_prefix('R').or_else(|| encoded.strip_prefix('r')) {
            let dlc = if dlc_text.is_empty() {
                0
            } else {
                dlc_text.parse::<u8>()
                    .map_err(|_| CandumpError::InvalidPayload(encoded.into()))?
            };
            (id, FrameKind::RemoteRequest { dlc }, Vec::new(), false, FdFlags::default())
        } else {
            (id, FrameKind::Data, hex_bytes(encoded)?, false, FdFlags::default())
        }
    } else {
        // The human-readable candump form (without the optional ASCII gutter).
        let id = parse_id(frame_token)?;
        let dlc_token = tokens.next().ok_or(CandumpError::Missing("[DLC]"))?;
        let dlc = dlc_token.strip_prefix('[').and_then(|s| s.strip_suffix(']'))
            .and_then(|s| s.parse::<usize>().ok())
            .ok_or_else(|| CandumpError::InvalidPayload(dlc_token.into()))?;
        let bytes: Vec<&str> = tokens.by_ref().collect();
        if bytes.len() != dlc {
            return Err(CandumpError::InvalidPayload(format!(
                "DLC {dlc} does not match {} bytes", bytes.len()
            )));
        }
        let mut data = Vec::with_capacity(dlc);
        for byte in bytes {
            if byte.len() != 2 {
                return Err(CandumpError::InvalidPayload(byte.into()));
            }
            data.push(u8::from_str_radix(byte, 16)
                .map_err(|_| CandumpError::InvalidPayload(byte.into()))?);
        }
        (id, FrameKind::Data, data, dlc > 8, FdFlags::default())
    };
    if frame_token.contains('#') && tokens.next().is_some() {
        return Err(CandumpError::InvalidPayload("unexpected trailing tokens".into()));
    }
    Ok(Some(CanFrame::new(
        timestamp_us, interface, id, kind, data, is_fd, flags
    )?))
}

/// Read all frames; fail with the source line number on malformed input.
pub fn read_candump<R: BufRead>(reader: R) -> Result<Vec<CanFrame>, TraceReadError> {
    let mut frames = Vec::new();
    for (index, line) in reader.lines().enumerate() {
        let line = line.map_err(TraceReadError::Io)?;
        if let Some(frame) = parse_candump_line(&line).map_err(|source| TraceReadError::Line {
            number: index + 1,
            source,
        })? {
            frames.push(frame);
        }
    }
    Ok(frames)
}
