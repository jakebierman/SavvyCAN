//! Receive-only LAWICEL/CAN232 (SLCAN) ASCII frame decoding.
//! Based on LAWICEL CAN232 v3 manual. No setup, mode-switch or transmit
//! commands are exposed, and the decoder never writes to a serial port.
use savvycan_core::{CanFrame, CanId, Direction, FdFlags, FrameKind};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SlcanDecodeError(pub String);
impl fmt::Display for SlcanDecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "invalid SLCAN frame: {}", self.0)
    }
}
impl std::error::Error for SlcanDecodeError {}

/// Handles CR-terminated t/T data and r/R remote request frames.
/// Optional four-digit Lawicel timestamps are milliseconds modulo 60,000.
/// Without a timestamp, the frame's time remains unknown.
#[derive(Debug)]
pub struct SlcanStreamDecoder {
    buffer: Vec<u8>,
    interface: String,
    last_raw_ms: Option<u16>,
    epoch_ms: u64,
    overflow: bool,
}
impl SlcanStreamDecoder {
    pub fn new(interface: impl Into<String>) -> Self {
        Self {
            buffer: Vec::with_capacity(40),
            interface: interface.into(),
            last_raw_ms: None,
            epoch_ms: 0,
            overflow: false,
        }
    }

    pub fn reset(&mut self) {
        self.buffer.clear();
        self.last_raw_ms = None;
        self.epoch_ms = 0;
        self.overflow = false;
    }

    fn parse_line(&mut self, line: &str) -> Option<Result<CanFrame, SlcanDecodeError>> {
        let marker = line.as_bytes().first().copied()?;
        let (extended, remote) = match marker {
            b't' => (false, false),
            b'T' => (true, false),
            b'r' => (false, true),
            b'R' => (true, true),
            // Device ACK, status, version, bitrate and alert lines are not CAN frames.
            _ => return None,
        };
        let id_digits = if extended { 8 } else { 3 };
        let minimum = 1 + id_digits + 1;
        if line.len() < minimum {
            return Some(Err(SlcanDecodeError("missing CAN identifier or DLC".into())));
        }
        let fail = |message: &str| SlcanDecodeError(message.into());
        let parse_hex = |value: &str| u32::from_str_radix(value, 16);
        let Some(id_text) = line.get(1..1 + id_digits) else {
            return Some(Err(fail("invalid CAN identifier field")));
        };
        let Ok(raw_id) = parse_hex(id_text) else {
            return Some(Err(fail("CAN identifier is not hexadecimal")));
        };
        let id = if extended {
            CanId::extended(raw_id)
        } else {
            u16::try_from(raw_id).map_err(|_| savvycan_core::FrameError::InvalidStandardId(
                u16::MAX
            )).and_then(CanId::standard)
        };
        let id = match id {
            Ok(id) => id,
            Err(error) => return Some(Err(SlcanDecodeError(error.to_string()))),
        };
        let Some(dlc_char) = line.as_bytes().get(1 + id_digits).copied() else {
            return Some(Err(fail("missing DLC")));
        };
        let dlc = match (dlc_char as char).to_digit(16) {
            Some(n @ 0..=8) => n as u8,
            _ => return Some(Err(fail("DLC exceeds classic CAN 8-byte limit"))),
        };
        let data_start = minimum;
        let data_end = data_start + if remote { 0 } else { usize::from(dlc) * 2 };
        if line.len() != data_end && line.len() != data_end + 4 {
            return Some(Err(fail("payload length or optional timestamp width is invalid")));
        }
        let mut data = Vec::with_capacity(usize::from(dlc));
        if !remote {
            for index in (data_start..data_end).step_by(2) {
                let Some(byte_text) = line.get(index..index + 2) else {
                    return Some(Err(fail("invalid payload character boundaries")));
                };
                match u8::from_str_radix(byte_text, 16) {
                    Ok(value) => data.push(value),
                    Err(_) => return Some(Err(fail("payload is not hexadecimal"))),
                }
            }
        }
        let timestamp_us = if line.len() == data_end + 4 {
            let Some(timestamp_text) = line.get(data_end..data_end + 4) else {
                return Some(Err(fail("invalid timestamp characters")));
            };
            let Ok(raw_ms) = u16::from_str_radix(timestamp_text, 16) else {
                return Some(Err(fail("timestamp is not hexadecimal")));
            };
            if raw_ms >= 60_000 {
                return Some(Err(fail("LAWICEL timestamp exceeds 60,000-ms modulus")));
            }
            if self.last_raw_ms.is_some_and(|last|
                raw_ms < last && last - raw_ms > 30_000
            ) {
                self.epoch_ms += 60_000;
            }
            self.last_raw_ms = Some(raw_ms);
            Some((self.epoch_ms + u64::from(raw_ms)) * 1000)
        } else { None };
        let kind = if remote { FrameKind::RemoteRequest { dlc } } else { FrameKind::Data };
        let mut frame = match CanFrame::new(
            timestamp_us, &self.interface, id, kind, data, false, FdFlags::default()
        ) {
            Ok(frame) => frame,
            Err(error) => return Some(Err(SlcanDecodeError(error.to_string()))),
        };
        frame.direction = Direction::Received;
        Some(Ok(frame))
    }

    /// Feed one arbitrary byte. Unknown non-frame responses are ignored.
    /// CR completes a record; LF after CR is harmless. Lines are bounded
    /// so a broken serial stream cannot grow memory without limit.
    pub fn push(&mut self, byte: u8) -> Option<Result<CanFrame, SlcanDecodeError>> {
        if byte == b'\n' && self.buffer.is_empty() { return None; }
        if byte == b'\r' {
            let was_overflow = std::mem::take(&mut self.overflow);
            let line = std::mem::take(&mut self.buffer);
            if was_overflow { return Some(Err(SlcanDecodeError("record exceeds 80 bytes".into()))); }
            let Ok(line) = std::str::from_utf8(&line) else {
                return Some(Err(SlcanDecodeError("record contains non-ASCII text".into())));
            };
            return self.parse_line(line);
        }
        if self.overflow { return None; }
        if self.buffer.len() == 80 {
            self.overflow = true;
            self.buffer.clear();
        } else {
            self.buffer.push(byte);
        }
        None
    }

    pub fn push_bytes(&mut self, bytes: &[u8])
        -> Vec<Result<CanFrame, SlcanDecodeError>>
    {
        bytes.iter().filter_map(|b| self.push(*b)).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn standard_extended_remote_and_chunked_ascii() {
        let mut parser = SlcanStreamDecoder::new("usbcan0");
        let stream = b"\r\nt12321122\rT000001238AABBCCDDEEFF0011\rr7E88\rR000007E82\r";
        let mut out = Vec::new();
        for chunk in stream.chunks(3) { out.extend(parser.push_bytes(chunk)); }
        assert_eq!(out.len(), 4);
        let frames = out.iter().map(|f| f.as_ref().unwrap()).collect::<Vec<_>>();
        assert_eq!(frames[0].id, CanId::Standard(0x123));
        assert_eq!(frames[0].data, vec![0x11, 0x22]);
        assert_eq!(frames[1].id, CanId::Extended(0x123));
        assert_eq!(frames[1].data.len(), 8);
        assert_eq!(frames[2].kind, FrameKind::RemoteRequest { dlc: 8 });
        assert_eq!(frames[3].kind, FrameKind::RemoteRequest { dlc: 2 });
        assert!(frames[3].data.is_empty());
        assert!(frames.iter().all(|f| f.interface == "usbcan0"
            && f.direction == Direction::Received));
    }

    #[test]
    fn optional_timestamp_rollover_and_unknown_time() {
        let mut parser = SlcanStreamDecoder::new("can0");
        let output = parser.push_bytes(b"t123101EA5E\rt1231020002\rt123103\r");
        assert_eq!(output.len(), 3);
        assert_eq!(output[0].as_ref().unwrap().timestamp_us, Some(59_998_000));
        assert_eq!(output[1].as_ref().unwrap().timestamp_us, Some(60_002_000));
        assert_eq!(output[2].as_ref().unwrap().timestamp_us, None);
    }

    #[test]
    fn ignores_ack_and_status_and_reports_bad_frames() {
        let mut parser = SlcanStreamDecoder::new("can0");
        let frames = parser.push_bytes(b"z\rZ\rF00\rV0101\rtXYZ1AB\rt1239\rt1232GG00\rr1238AA\rt123101\r");
        assert_eq!(frames.len(), 5);
        assert!(frames[0].is_err());
        assert!(frames[1].is_err());
        assert!(frames[2].is_err());
        assert!(frames[3].is_err());
        assert_eq!(frames[4].as_ref().unwrap().data, vec![1]);
    }

    #[test]
    fn length_cap_and_reset_recover() {
        let mut parser = SlcanStreamDecoder::new("can0");
        let mut oversized = vec![b'X'; 90];
        oversized.push(b'\r');
        assert!(parser.push_bytes(&oversized)[0].is_err());
        parser.push_bytes(b"t1232");
        parser.reset();
        assert_eq!(parser.push_bytes(b"t1230\r")[0].as_ref().unwrap().data, Vec::<u8>::new());
    }
}
