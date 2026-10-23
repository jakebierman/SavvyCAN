//! Text capture import/export compatible with SavvyCAN's CRTD and native CSV formats.
//! Detection uses file contents, not an ambiguous filename extension.
use crate::{parse_candump_line, CanFrame, CanId, Direction, FdFlags, FrameKind};
use std::fmt;
use std::io::{self, BufRead, Write};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraceFormat {
    Candump,
    Crtd,
    GvretCsv,
    GenericCsv,
}

impl fmt::Display for TraceFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Candump => "candump",
            Self::Crtd => "CRTD",
            Self::GvretCsv => "GVRET CSV",
            Self::GenericCsv => "generic ID/data CSV",
        })
    }
}

#[derive(Debug)]
pub struct ImportedTrace {
    pub format: TraceFormat,
    pub frames: Vec<CanFrame>,
}

#[derive(Debug)]
pub enum ImportError {
    Io(io::Error),
    Empty,
    Unsupported(String),
    Line { number: usize, reason: String },
}

impl fmt::Display for ImportError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(err) => err.fmt(f),
            Self::Empty => write!(f, "trace is empty"),
            Self::Unsupported(line) => write!(f, "unsupported trace format near: {line}"),
            Self::Line { number, reason } => write!(f, "trace line {number}: {reason}"),
        }
    }
}
impl std::error::Error for ImportError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self { Self::Io(err) => Some(err), _ => None }
    }
}
impl From<io::Error> for ImportError {
    fn from(err: io::Error) -> Self { Self::Io(err) }
}

#[derive(Debug)]
pub enum ExportError {
    Io(io::Error),
    Unsupported { index: usize, reason: String },
}
impl fmt::Display for ExportError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(err) => err.fmt(f),
            Self::Unsupported { index, reason } => write!(f, "frame {}: {reason}", index + 1),
        }
    }
}
impl std::error::Error for ExportError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self { Self::Io(err) => Some(err), _ => None }
    }
}
impl From<io::Error> for ExportError {
    fn from(err: io::Error) -> Self { Self::Io(err) }
}

fn detect(first: &str) -> Result<TraceFormat, ImportError> {
    let line = first.trim_start_matches('\u{feff}').trim();
    let normalized = line.to_ascii_lowercase();
    if normalized.starts_with("time stamp,id,extended,") {
        Ok(TraceFormat::GvretCsv)
    } else if normalized.starts_with("id,data bytes") {
        Ok(TraceFormat::GenericCsv)
    } else {
        let mut words = line.split_whitespace();
        let timestamp = words.next().unwrap_or("");
        let marker = words.next().unwrap_or("");
        let marker = marker.trim_start_matches(|c: char| c.is_ascii_digit());
        if !timestamp.is_empty() && (marker == "CXX" || marker == "CEV"
            || matches!(marker, "R11" | "R29" | "T11" | "T29")) {
            Ok(TraceFormat::Crtd)
        } else if line.starts_with('#') || line.starts_with('(')
            || line.contains("can") || line.contains("vcan") {
            Ok(TraceFormat::Candump)
        } else {
            Err(ImportError::Unsupported(line.chars().take(100).collect()))
        }
    }
}

/// Import one supported trace without silently dropping malformed data rows.
/// CRTD event/comment records do not contain CAN frames and are intentionally ignored.
pub fn read_trace<R: BufRead>(reader: R) -> Result<ImportedTrace, ImportError> {
    let mut format = None;
    let mut gvret_v2 = false;
    let mut frames = Vec::new();
    for (index, line) in reader.lines().enumerate() {
        let line = line?;
        let trimmed = line.trim_start_matches('\u{feff}').trim();
        if trimmed.is_empty() { continue; }
        if format.is_none() {
            format = Some(detect(trimmed)?);
            if format == Some(TraceFormat::GvretCsv) {
                gvret_v2 = trimmed.split(',').any(|cell| cell.trim().eq_ignore_ascii_case("Dir"));
            }
        }
        let result = match format.expect("selected above") {
            TraceFormat::Candump => parse_candump_line(trimmed)
                .map_err(|error| error.to_string()),
            TraceFormat::Crtd => parse_crtd_line(trimmed),
            TraceFormat::GvretCsv => {
                if trimmed.to_ascii_lowercase().starts_with("time stamp,") {
                    Ok(None)
                } else { parse_gvret_csv_line(trimmed, gvret_v2) }
            }
            TraceFormat::GenericCsv => {
                if trimmed.to_ascii_lowercase().starts_with("id,data bytes") {
                    Ok(None)
                } else { parse_generic_csv_line(trimmed) }
            }
        };
        match result {
            Ok(Some(frame)) => frames.push(frame),
            Ok(None) => {}
            Err(reason) => return Err(ImportError::Line { number: index + 1, reason }),
        }
    }
    let format = format.ok_or(ImportError::Empty)?;
    Ok(ImportedTrace { format, frames })
}

fn parse_hex_id(raw: &str, extended: bool) -> Result<CanId, String> {
    let value = u32::from_str_radix(raw.trim_start_matches("0x").trim_start_matches("0X"), 16)
        .map_err(|_| format!("invalid hex CAN identifier '{raw}'"))?;
    if extended || value > CanId::MAX_STANDARD as u32 {
        CanId::extended(value).map_err(|err| err.to_string())
    } else {
        CanId::standard(value as u16).map_err(|err| err.to_string())
    }
}

fn parse_hex_byte(raw: &str) -> Result<u8, String> {
    let digits = raw.trim_start_matches("0x").trim_start_matches("0X");
    if digits.is_empty() || digits.len() > 2 {
        return Err(format!("invalid byte '{raw}'"));
    }
    u8::from_str_radix(digits, 16)
        .map_err(|_| format!("invalid hex byte '{raw}'"))
}

fn parse_timestamp(raw: &str) -> Result<u64, String> {
    if let Some((seconds, fraction)) = raw.split_once('.') {
        if fraction.len() > 9 || !fraction.bytes().all(|byte| byte.is_ascii_digit()) {
            return Err(format!("invalid timestamp '{raw}'"));
        }
        let sec = seconds.parse::<u64>().map_err(|_| format!("invalid timestamp '{raw}'"))?;
        let take = fraction.len().min(6);
        let us = if take == 0 { 0 } else {
            fraction[..take].parse::<u64>().map_err(|_| format!("invalid timestamp '{raw}'"))?
                * 10u64.pow((6 - take) as u32)
        };
        sec.checked_mul(1_000_000).and_then(|s| s.checked_add(us))
            .ok_or_else(|| format!("timestamp outside u64 range '{raw}'"))
    } else {
        // Legacy CRTD logs with no decimal use raw microseconds.
        raw.parse::<u64>().map_err(|_| format!("invalid timestamp '{raw}'"))
    }
}

fn frame(timestamp_us: Option<u64>, interface: String, id: CanId, payload: Vec<u8>, direction: Direction) -> Result<CanFrame, String> {
    let mut frame = CanFrame::new(timestamp_us, interface, id, FrameKind::Data, payload, false, FdFlags::default())
        .map_err(|err| err.to_string())?;
    frame.direction = direction;
    Ok(frame)
}

fn parse_crtd_line(line: &str) -> Result<Option<CanFrame>, String> {
    let mut words = line.split_whitespace();
    let timestamp = words.next().ok_or("missing CRTD timestamp")?;
    let marker = words.next().ok_or("missing CRTD record type")?;
    let unprefixed = marker.trim_start_matches(|c: char| c.is_ascii_digit());
    if matches!(unprefixed, "CXX" | "CEV") {
        return Ok(None);
    }
    if !matches!(unprefixed, "R11" | "R29" | "T11" | "T29") {
        return Err(format!("unsupported CRTD record type '{marker}'"));
    }
    let numeric_prefix = &marker[..marker.len() - unprefixed.len()];
    let bus_index = if numeric_prefix.is_empty() { 0 } else {
        let bus = numeric_prefix.parse::<usize>().map_err(|_| "invalid CRTD bus number")?;
        if !(1..=255).contains(&bus) { return Err("CRTD bus number must be 1..255".into()); }
        bus - 1
    };
    let id = words.next().ok_or("missing CRTD CAN identifier")?;
    let explicit_extended = unprefixed.ends_with("29");
    let parsed_id = parse_hex_id(id, explicit_extended)?;
    if !explicit_extended && parsed_id.is_extended() {
        return Err("11-bit CRTD record has an extended identifier".into());
    }
    let data = words.map(parse_hex_byte).collect::<Result<Vec<_>, _>>()?;
    let direction = if unprefixed.starts_with('R') { Direction::Received } else { Direction::Transmitted };
    Ok(Some(frame(Some(parse_timestamp(timestamp)?), format!("bus{bus_index}"), parsed_id, data, direction)?))
}

fn parse_gvret_csv_line(line: &str, v2: bool) -> Result<Option<CanFrame>, String> {
    let values: Vec<&str> = line.split(',').map(str::trim).collect();
    let minimum = if v2 { 6 } else { 5 };
    if values.len() < minimum { return Err("GVRET CSV is missing required columns".into()); }
    let timestamp = values[0].parse::<u64>().map_err(|_| "invalid GVRET timestamp")?;
    let extended = match values[2].to_ascii_lowercase().as_str() {
        "true" | "1" => true,
        "false" | "0" => false,
        _ => return Err("GVRET Extended column must be true/false".into()),
    };
    let id = parse_hex_id(values[1], extended)?;
    let direction = if v2 {
        match values[3].to_ascii_lowercase().as_str() {
            "rx" | "r" => Direction::Received,
            "tx" | "t" => Direction::Transmitted,
            _ => return Err("GVRET Dir column must be Rx/Tx".into()),
        }
    } else { Direction::Received };
    let bus_index = if v2 { 4 } else { 3 };
    let bus = values[bus_index].parse::<u16>().map_err(|_| "invalid GVRET bus number")?;
    let len = values[bus_index + 1].parse::<usize>().map_err(|_| "invalid GVRET length")?;
    if len > 8 { return Err("GVRET classic CSV length cannot exceed 8".into()); }
    let first_data = bus_index + 2;
    if values.len() < first_data + len { return Err("GVRET CSV has fewer data bytes than LEN".into()); }
    let mut data = Vec::with_capacity(len);
    for &byte in &values[first_data..first_data+len] { data.push(parse_hex_byte(byte)?); }
    Ok(Some(frame(Some(timestamp), format!("bus{bus}"), id, data, direction)?))
}

fn parse_generic_csv_line(line: &str) -> Result<Option<CanFrame>, String> {
    let (id, data) = line.split_once(',').ok_or("generic CSV row requires ID and data")?;
    let id = parse_hex_id(id.trim(), false)?;
    let data = data.split_whitespace().map(parse_hex_byte).collect::<Result<Vec<_>, _>>()?;
    Ok(Some(frame(None, "bus0".into(), id, data, Direction::Unknown)?))
}

fn export_bus(frame: &CanFrame, index: usize) -> Result<u16, ExportError> {
    frame.interface.strip_prefix("bus")
        .or_else(|| frame.interface.strip_prefix("can"))
        .and_then(|s| s.parse::<u16>().ok())
        .ok_or_else(|| ExportError::Unsupported {
            index, reason: format!("interface '{}' cannot be represented as a GVRET bus number", frame.interface)
        })
}

fn preflight(frames: &[CanFrame], crt: bool) -> Result<(), ExportError> {
    for (index, frame) in frames.iter().enumerate() {
        if frame.is_fd || !matches!(frame.kind, FrameKind::Data) {
            return Err(ExportError::Unsupported { index, reason: "classic-only format cannot represent CAN FD or RTR".into() });
        }
        if frame.timestamp_us.is_none() {
            return Err(ExportError::Unsupported { index, reason: "timestamp is required for this format".into() });
        }
        if matches!(frame.direction, Direction::Unknown) {
            return Err(ExportError::Unsupported { index, reason: "Rx/Tx direction must be known for lossless export".into() });
        }
        let bus = export_bus(frame, index)?;
        if crt && bus >= 255 {
            return Err(ExportError::Unsupported { index, reason: "CRTD bus number exceeds 255".into() });
        }
    }
    Ok(())
}

/// Write CRTD with microsecond timestamps, preserving bus, frame format and direction.
/// Unsupported frames are rejected before the first byte is written.
pub fn write_crtd<W: Write>(mut writer: W, frames: &[CanFrame]) -> Result<(), ExportError> {
    preflight(frames, true)?;
    let first = frames.first().and_then(|f| f.timestamp_us).unwrap_or_default();
    writeln!(writer, "{}.{:06} CXX SavvyCAN Rust export", first / 1_000_000, first % 1_000_000)?;
    for (index, frame) in frames.iter().enumerate() {
        let stamp = frame.timestamp_us.expect("preflight");
        let bus = export_bus(frame, index)? + 1;
        let dir = if matches!(frame.direction, Direction::Received) { 'R' } else { 'T' };
        let width = if frame.id.is_extended() { 29 } else { 11 };
        write!(writer, "{}.{:06} {bus}{dir}{width} {:08X}",
            stamp / 1_000_000, stamp % 1_000_000, frame.id.value())?;
        for byte in &frame.data { write!(writer, " {byte:02X}")?; }
        writeln!(writer)?;
    }
    Ok(())
}

/// Write SavvyCAN/GVRET CSV V2. Classic CAN only, with Rx/Tx and bus identity.
pub fn write_gvret_csv<W: Write>(mut writer: W, frames: &[CanFrame]) -> Result<(), ExportError> {
    preflight(frames, false)?;
    writeln!(writer, "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8")?;
    for (index, frame) in frames.iter().enumerate() {
        let dir = if matches!(frame.direction, Direction::Received) { "Rx" } else { "Tx" };
        let bus = export_bus(frame, index)?;
        write!(writer, "{},{:08X},{},{dir},{bus},{}", frame.timestamp_us.expect("preflight"),
            frame.id.value(), frame.id.is_extended(), frame.data.len())?;
        for index in 0..8 {
            write!(writer, ",{:02X}", frame.data.get(index).copied().unwrap_or_default())?;
        }
        writeln!(writer)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn load(contents: &str) -> ImportedTrace { read_trace(Cursor::new(contents)).unwrap() }

    #[test]
    fn detect_and_import_original_savvycan_examples() {
        let crt = load(include_str!("../../../examples/CRTD_Log.txt"));
        let csv = load(include_str!("../../../examples/GVRET_Log.csv"));
        let generic = load(include_str!("../../../examples/GenericID_Log.csv"));
        assert_eq!(crt.format, TraceFormat::Crtd);
        assert_eq!(csv.format, TraceFormat::GvretCsv);
        assert_eq!(generic.format, TraceFormat::GenericCsv);
        assert!(crt.frames.len() > 100);
        assert_eq!(crt.frames.len(), csv.frames.len());
        assert_eq!(crt.frames.len(), generic.frames.len());
        assert_eq!(crt.frames[0], csv.frames[0]);
        assert_eq!(crt.frames[0].id, generic.frames[0].id);
        assert_eq!(crt.frames[0].data, generic.frames[0].data);
        assert_eq!(crt.frames[0].timestamp_us, Some(166_064_000));
    }

    #[test]
    fn gvret_v2_preserves_extended_standard_id_distinction_and_direction() {
        let text = "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n\
            12,00000123,true,Tx,3,2,01,FF\n\
            34,00000123,false,Rx,0,0\n";
        let imported = load(text);
        assert_eq!(imported.frames.len(), 2);
        assert_eq!(imported.frames[0].id, CanId::Extended(0x123));
        assert_eq!(imported.frames[0].interface, "bus3");
        assert_eq!(imported.frames[0].direction, Direction::Transmitted);
        assert_eq!(imported.frames[1].id, CanId::Standard(0x123));
        assert_eq!(imported.frames[1].direction, Direction::Received);
    }

    #[test]
    fn crtd_optional_bus_comments_and_microsecond_integer_timestamp() {
        let text = "0 CXX header\n123 2R29 00000123 AA BB\n234 CEV event\n250 R11 7FF 00\n";
        let parsed = load(text);
        assert_eq!(parsed.frames.len(), 2);
        assert_eq!(parsed.frames[0].timestamp_us, Some(123));
        assert_eq!(parsed.frames[0].interface, "bus1");
        assert_eq!(parsed.frames[0].id, CanId::Extended(0x123));
        assert_eq!(parsed.frames[1].interface, "bus0");
        assert_eq!(parsed.frames[1].id, CanId::Standard(0x7ff));
    }

    #[test]
    fn crt_and_csv_round_trip() {
        let original = load(include_str!("../../../examples/CRTD_Log.txt")).frames;
        let sample = &original[..50];
        let mut bytes = Vec::new();
        write_crtd(&mut bytes, sample).unwrap();
        let from_crt = read_trace(Cursor::new(&bytes)).unwrap().frames;
        assert_eq!(from_crt, sample);
        bytes.clear();
        write_gvret_csv(&mut bytes, sample).unwrap();
        let from_csv = read_trace(Cursor::new(&bytes)).unwrap().frames;
        assert_eq!(from_csv, sample);
    }

    #[test]
    fn bad_input_reports_line_numbers_and_rejects_invalid_lengths() {
        let invalid = "Time Stamp,ID,Extended,Bus,LEN,D1\n10,123,false,0,2,AA\n";
        let err = read_trace(Cursor::new(invalid)).unwrap_err();
        assert!(err.to_string().contains("line 2"), "{err}");
        let invalid = "10 R11 123 ZZ\n";
        assert!(read_trace(Cursor::new(invalid)).unwrap_err().to_string().contains("line 1"));
        let invalid = "0 CXX header\n10 R11 800 AA\n";
        assert!(read_trace(Cursor::new(invalid)).is_err());
    }

    #[test]
    fn export_rejects_inexpressible_frames_without_writing() {
        let mut frame = frame(Some(0), "bus0".into(), CanId::Standard(0x100),
            vec![0xAA], Direction::Unknown).unwrap();
        let mut bytes = Vec::new();
        assert!(write_gvret_csv(&mut bytes, &[frame.clone()]).is_err());
        assert!(bytes.is_empty());
        frame.direction = Direction::Received;
        frame.is_fd = true;
        assert!(write_crtd(&mut bytes, &[frame]).is_err());
        assert!(bytes.is_empty());
    }

    #[test]
    fn candump_remains_auto_detected() {
        let frames = load("(12.123456) can0 123#AABB\n");
        assert_eq!(frames.format, TraceFormat::Candump);
        assert_eq!(frames.frames[0].timestamp_us, Some(12_123_456));
    }
}
