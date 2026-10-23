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
    Busmaster,
    Microchip,
    CarbusAnalyzer,
    VectorAsc,
    PcanTrc,
    IxxatMinilog,
    VehicleSpy,
    SocketCanPcap,
    SocketCanPcapNg,
    CabanaCsv,
}

impl fmt::Display for TraceFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Candump => "candump",
            Self::Crtd => "CRTD",
            Self::GvretCsv => "GVRET CSV",
            Self::GenericCsv => "generic ID/data CSV",
            Self::Busmaster => "BUSMASTER log",
            Self::Microchip => "Microchip CAN BUS Analyzer log",
            Self::CarbusAnalyzer => "CARBUS Analyzer trace",
            Self::VectorAsc => "Vector CANalyzer ASC (classic)",
            Self::PcanTrc => "PEAK PCAN TRC",
            Self::IxxatMinilog => "IXXAT MiniLog CSV",
            Self::VehicleSpy => "Vehicle Spy CSV",
            Self::SocketCanPcap => "SocketCAN PCAP",
            Self::SocketCanPcapNg => "SocketCAN PCAPNG",
            Self::CabanaCsv => "Cabana CSV",
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
    if normalized.starts_with("time,addr,bus,data") {
        Ok(TraceFormat::CabanaCsv)
    } else if normalized.starts_with("line,") && normalized.contains("abs time") && normalized.contains("b1") {
        Ok(TraceFormat::VehicleSpy)
    } else if normalized.starts_with("ascii trace ixxat") {
        Ok(TraceFormat::IxxatMinilog)
    } else if normalized.starts_with(";$fileversion=") {
        Ok(TraceFormat::PcanTrc)
    } else if normalized.starts_with("date ") || normalized.starts_with("base hex ") {
        Ok(TraceFormat::VectorAsc)
    } else if normalized.starts_with("@ text @") {
        Ok(TraceFormat::CarbusAnalyzer)
    } else if normalized.starts_with("***busmaster") {
        Ok(TraceFormat::Busmaster)
    } else if normalized.starts_with("//---") || normalized.starts_with("microchip technology inc") {
        Ok(TraceFormat::Microchip)
    } else if normalized.starts_with("time stamp,id,extended,") {
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
pub fn read_trace<R: BufRead>(mut reader: R) -> Result<ImportedTrace, ImportError> {
    if reader.fill_buf()?.starts_with(&[0x0a, 0x0d, 0x0d, 0x0a]) {
        return crate::pcapng::read_socketcan_pcapng(reader);
    }
    if crate::pcap::is_pcap_magic(reader.fill_buf()?) {
        return crate::pcap::read_socketcan_pcap(reader);
    }
    let mut format = None;
    let mut gvret_v2 = false;
    let mut carbus_v2 = false;
    let mut pcan_version: Option<u8> = None;
    let mut pcan_columns: Vec<String> = Vec::new();
    let mut frames = Vec::new();
    let mut line_number = 0usize;
    for physical_line in reader.lines() {
        let physical_line = physical_line?;
        // Some CARBUS Analyzer files delimit records using CR only.
        for line in physical_line.split('\r') {
            line_number += 1;
            let trimmed = line.trim_start_matches('\u{feff}').trim();
            if trimmed.is_empty() { continue; }
            if format.is_none() {
                format = Some(detect(trimmed)?);
                if format == Some(TraceFormat::GvretCsv) {
                    gvret_v2 = trimmed.split(',').any(|cell| cell.trim().eq_ignore_ascii_case("Dir"));
                }
                if format == Some(TraceFormat::CarbusAnalyzer) {
                    carbus_v2 = trimmed.split('@').nth(2).is_some_and(|v| v.trim() == "2");
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
                TraceFormat::Busmaster => {
                    if trimmed.starts_with("***") { Ok(None) }
                    else { parse_busmaster_line(trimmed) }
                }
                TraceFormat::Microchip => {
                    if trimmed.starts_with("//") || !trimmed.contains(';') { Ok(None) }
                    else { parse_microchip_line(trimmed) }
                }
                TraceFormat::CarbusAnalyzer => {
                    if trimmed.starts_with("@ TEXT @") { Ok(None) }
                    else { parse_carbus_line(trimmed, carbus_v2) }
                }
                TraceFormat::VectorAsc => parse_vector_asc_line(trimmed),
                TraceFormat::IxxatMinilog => parse_ixxat_line(trimmed),
                TraceFormat::VehicleSpy => parse_vehicle_spy_line(trimmed),
                TraceFormat::CabanaCsv => {
                    if trimmed.to_ascii_lowercase().starts_with("time,addr,bus,data") {
                        Ok(None)
                    } else { parse_cabana_csv_line(trimmed) }
                },
                TraceFormat::SocketCanPcap | TraceFormat::SocketCanPcapNg =>
                    unreachable!("binary PCAP/PCAPNG was detected first"),
                TraceFormat::PcanTrc => {
                    if let Some(value) = trimmed.strip_prefix(";$FILEVERSION=") {
                        pcan_version = match value.trim().trim_end_matches(';') {
                            "1.1" => Some(11), "1.2" => Some(12), "1.3" => Some(13),
                            "2.0" => Some(20), "2.1" => Some(21),
                            _ => return Err(ImportError::Line { number: line_number,
                                reason: format!("unsupported PCAN TRC version '{value}'") }),
                        };
                        Ok(None)
                    } else if let Some(value) = trimmed.strip_prefix(";$COLUMNS=") {
                        pcan_columns = value.split(',').map(|v| v.trim().to_owned()).collect();
                        validate_pcan_columns(&pcan_columns).map(|()| None)
                    } else if trimmed.starts_with(';') {
                        Ok(None)
                    } else {
                        let version = pcan_version.ok_or_else(|| "PCAN TRC missing $FILEVERSION header".to_string());
                        version.and_then(|v| parse_pcan_line(trimmed, v, &pcan_columns))
                    }
                },
            };
            match result {
                Ok(Some(frame)) => frames.push(frame),
                Ok(None) => {}
                Err(reason) => return Err(ImportError::Line { number: line_number, reason }),
            }
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





fn pcan_millis_to_us(raw: &str) -> Result<u64, String> {
    let (ms, fraction) = raw.split_once('.').unwrap_or((raw, ""));
    if fraction.len() > 3 || !fraction.bytes().all(|b| b.is_ascii_digit()) {
        return Err(format!("invalid PCAN millisecond timestamp '{raw}'"));
    }
    let base = ms.parse::<u64>().map_err(|_| format!("invalid PCAN timestamp '{raw}'"))?;
    let micro_fraction = if fraction.is_empty() { 0 } else {
        fraction.parse::<u64>().map_err(|_| "invalid PCAN sub-millisecond digits")?
            * 10_u64.pow((3 - fraction.len()) as u32)
    };
    base.checked_mul(1000).and_then(|v| v.checked_add(micro_fraction))
        .ok_or_else(|| "PCAN timestamp outside u64 range".into())
}

fn pcan_id(raw: &str) -> Result<CanId, String> {
    if raw.len() != 4 && raw.len() != 8 {
        return Err("PCAN CAN IDs must be exactly four or eight hexadecimal digits".into());
    }
    let id = parse_hex_id(raw, raw.len() == 8)?;
    if raw.len() == 4 && id.is_extended() {
        return Err("PCAN four-digit CAN ID exceeds the standard range".into());
    }
    Ok(id)
}

fn pcan_frame(
    timestamp: u64, bus: String, id: CanId, direction: Direction,
    kind: FrameKind, payload: Vec<u8>, fd_flags: FdFlags, is_fd: bool,
) -> Result<Option<CanFrame>, String> {
    let mut result = CanFrame::new(Some(timestamp), bus, id, kind, payload, is_fd, fd_flags)
        .map_err(|e| e.to_string())?;
    result.direction = direction;
    Ok(Some(result))
}

fn pcan_direction(text: &str) -> Result<Direction, String> {
    match text {
        "Rx" => Ok(Direction::Received),
        "Tx" => Ok(Direction::Transmitted),
        _ => Err(format!("invalid PCAN direction '{text}'")),
    }
}

fn validate_pcan_columns(cols: &[String]) -> Result<(), String> {
    for name in cols {
        if !matches!(name.as_str(), "N" | "O" | "T" | "B" | "I" | "d" | "R" | "l" | "L" | "D") {
            return Err(format!("unsupported PCAN TRC column '{name}'"));
        }
    }
    for required in ["O", "T", "I", "d", "D"] {
        if !cols.iter().any(|c| c == required) {
            return Err(format!("PCAN TRC $COLUMNS missing '{required}'"));
        }
    }
    if !cols.iter().any(|c| c == "l" || c == "L") {
        return Err("PCAN TRC needs a data length or DLC column".into());
    }
    if !cols.last().is_some_and(|c| c == "D") {
        return Err("PCAN TRC data column must be last".into());
    }
    if cols.iter().collect::<std::collections::HashSet<_>>().len() != cols.len() {
        return Err("duplicate PCAN TRC column".into());
    }
    Ok(())
}

fn pcan_fd_dlc_bytes(dlc: usize) -> Result<usize, String> {
    const LENGTHS: [usize; 16] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64];
    LENGTHS.get(dlc).copied().ok_or("PCAN CAN FD DLC exceeds 15".into())
}



/// Read CSV cells with escaped quotes; descriptions may contain commas.

fn parse_cabana_csv_line(line: &str) -> Result<Option<CanFrame>, String> {
    let columns: Vec<&str> = line.split(',').map(str::trim).collect();
    if columns.len() != 4 {
        return Err("Cabana CSV requires time,addr,bus,data columns".into());
    }
    let timestamp_us = parse_timestamp(columns[0])?;
    let numeric_id = columns[1].parse::<u32>()
        .map_err(|_| "Cabana CAN ID must be decimal")?;
    let id = if numeric_id <= u32::from(CanId::MAX_STANDARD) {
        CanId::standard(numeric_id as u16)
    } else { CanId::extended(numeric_id) }.map_err(|err| err.to_string())?;
    let bus = columns[2].parse::<u16>().map_err(|_| "invalid Cabana bus number")?;
    let raw = columns[3];
    if raw.len() % 2 != 0 || !raw.is_ascii() {
        return Err("Cabana payload must contain pairs of hexadecimal digits".into());
    }
    let mut data = Vec::with_capacity(raw.len() / 2);
    for byte in raw.as_bytes().chunks_exact(2) {
        let hex = std::str::from_utf8(byte).expect("ASCII checked");
        data.push(parse_hex_byte(hex)?);
    }
    if data.len() > 8 {
        return Err("Cabana format does not encode CAN FD flags; payload above 8 bytes rejected".into());
    }
    Ok(Some(frame(Some(timestamp_us), format!("bus{bus}"),
        id, data, Direction::Unknown)?))
}

fn quoted_csv_cells(line: &str) -> Result<Vec<String>, String> {
    let mut cells = Vec::new();
    let mut current = String::new();
    let mut chars = line.chars().peekable();
    let mut in_quotes = false;
    while let Some(ch) = chars.next() {
        match ch {
            '"' if in_quotes && chars.peek() == Some(&'"') => {
                current.push('"');
                chars.next();
            }
            '"' => in_quotes = !in_quotes,
            ',' if !in_quotes => {
                cells.push(std::mem::take(&mut current));
            }
            _ => current.push(ch),
        }
    }
    if in_quotes {
        return Err("unclosed quoted Vehicle Spy CSV field".into());
    }
    cells.push(current);
    Ok(cells)
}

fn parse_vehicle_spy_line(line: &str) -> Result<Option<CanFrame>, String> {
    if line.to_ascii_lowercase().starts_with("line,") {
        return Ok(None);
    }
    let columns = quoted_csv_cells(line)?;
    if columns.len() < 23 {
        return Err("Vehicle Spy frame requires at least 23 columns".into());
    }
    // The source 'Abs Time' is already seconds; never synthesize a wall clock.
    let timestamp = parse_timestamp(columns[1].trim())?;
    let direction = match columns[5].trim().to_ascii_uppercase().as_str() {
        "T" => Direction::Transmitted,
        "F" => Direction::Received,
        _ => return Err("Vehicle Spy Tx field must be T or F".into()),
    };
    if columns[10].trim().eq_ignore_ascii_case("T") {
        return Err("Vehicle Spy remote requests need a DLC unavailable in this CSV row".into());
    }
    let extended = match columns[11].trim().to_ascii_uppercase().as_str() {
        "T" => true,
        "F" => false,
        _ => return Err("Vehicle Spy Xtd field must be T or F".into()),
    };
    let id = parse_hex_id(columns[9].trim(), extended)?;
    if !extended && id.is_extended() {
        return Err("Vehicle Spy standard frame ID exceeds 7FF".into());
    }
    let mut data = Vec::new();
    let mut reached_padding = false;
    for cell in &columns[12..20] {
        if cell.trim().is_empty() {
            reached_padding = true;
        } else {
            if reached_padding {
                return Err("Vehicle Spy payload contains a byte after a blank position".into());
            }
            data.push(parse_hex_byte(cell.trim())?);
        }
    }
    Ok(Some(frame(Some(timestamp), "bus0".into(), id, data, direction)?))
}

fn ixxat_clock_us(raw: &str) -> Result<u64, String> {
    let parts: Vec<&str> = raw.split(':').collect();
    if parts.len() != 3 {
        return Err("IXXAT timestamp must have hour:minute:second.fraction".into());
    }
    let hours = parts[0].parse::<u64>().map_err(|_| "invalid IXXAT hour")?;
    let minutes = parts[1].parse::<u64>().map_err(|_| "invalid IXXAT minute")?;
    let (secs, fraction) = parts[2].split_once('.').unwrap_or((parts[2], ""));
    let seconds = secs.parse::<u64>().map_err(|_| "invalid IXXAT second")?;
    if hours > 23 || minutes > 59 || seconds > 59 || fraction.len() > 6
        || !fraction.bytes().all(|b| b.is_ascii_digit()) {
        return Err("IXXAT timestamp is outside the clock range".into());
    }
    let microseconds = if fraction.is_empty() { 0 } else {
        fraction.parse::<u64>().map_err(|_| "invalid IXXAT sub-second digits")?
            * 10_u64.pow((6 - fraction.len()) as u32)
    };
    Ok(((hours * 60 + minutes) * 60 + seconds) * 1_000_000 + microseconds)
}

fn parse_ixxat_line(line: &str) -> Result<Option<CanFrame>, String> {
    // SavvyCAN's IXXAT MiniLog writer emits six metadata lines and one
    // quoted CSV heading before its five quoted data columns.
    if !line.starts_with('"') || line.starts_with("\"Time\"") {
        return Ok(None);
    }
    let content = line.strip_prefix('"').and_then(|v| v.strip_suffix('"'))
        .ok_or("IXXAT CSV row must have quoted fields")?;
    let fields: Vec<&str> = content.split("\",\"").collect();
    if fields.len() != 5 {
        return Err("IXXAT CSV row must have five quoted fields".into());
    }
    let timestamp = ixxat_clock_us(fields[0])?;
    let extended = match fields[2].to_ascii_lowercase().as_str() {
        "std" => false, "ext" => true,
        other => return Err(format!("unsupported IXXAT frame format '{other}'")),
    };
    let id = parse_hex_id(fields[1], extended)?;
    if !extended && id.is_extended() {
        return Err("IXXAT Std frame has CAN ID above 7FF".into());
    }
    if !fields[3].trim().is_empty() {
        return Err("IXXAT frame flags are not yet supported".into());
    }
    let payload = fields[4].split_whitespace().map(parse_hex_byte)
        .collect::<Result<Vec<_>, _>>()?;
    // IXXAT CSV does not encode Rx/Tx direction or bus in each data row.
    Ok(Some(frame(Some(timestamp), "bus0".into(),
        id, payload, Direction::Unknown)?))
}

fn parse_pcan_line(line: &str, version: u8, columns: &[String]) -> Result<Option<CanFrame>, String> {
    let words: Vec<&str> = line.split_whitespace().collect();
    if words.is_empty() { return Ok(None); }
    if version < 20 {
        if !words[0].ends_with(')') {
            return Err("PCAN 1.x record must start with a numbered 'N)' field".into());
        }
        let timestamp = pcan_millis_to_us(*words.get(1).ok_or("PCAN timestamp missing")?)?;
        let (bus, type_pos) = if version >= 12 {
            let number = words.get(2).ok_or("PCAN bus number missing")?
                .parse::<u16>().map_err(|_| "invalid PCAN bus number")?;
            if !(1..=16).contains(&number) { return Err("PCAN bus must be 1..16".into()); }
            (format!("bus{}", number - 1), 3)
        } else { ("bus0".into(), 2) };
        let type_field = *words.get(type_pos).ok_or("PCAN message type missing")?;
        let direction = match type_field {
            "Rx" | "Tx" => pcan_direction(type_field)?,
            "Warng" | "Error" => return Ok(None), // not representable as a CAN data frame
            _ => return Err(format!("unknown PCAN 1.x message type '{type_field}'")),
        };
        let id = pcan_id(words.get(type_pos + 1).ok_or("PCAN CAN ID missing")?)?;
        let len_pos = type_pos + if version >= 13 { 3 } else { 2 };
        let length = words.get(len_pos).ok_or("PCAN DLC missing")?
            .parse::<u8>().map_err(|_| "invalid PCAN DLC")?;
        if length > 8 { return Err("PCAN J1939 large messages require reassembly support".into()); }
        let data = &words[len_pos + 1..];
        if data == ["RTR"] {
            return pcan_frame(timestamp, bus, id, direction, FrameKind::RemoteRequest { dlc: length },
                Vec::new(), FdFlags::default(), false);
        }
        if data.len() != usize::from(length) {
            return Err(format!("PCAN DLC {length} does not match {} payload bytes", data.len()));
        }
        let payload = data.iter().map(|word| parse_hex_byte(word))
            .collect::<Result<Vec<_>, _>>()?;
        return pcan_frame(timestamp, bus, id, direction, FrameKind::Data,
            payload, FdFlags::default(), false);
    }

    if columns.is_empty() { return Err("PCAN 2.x missing $COLUMNS header".into()); }
    let mut timestamp = None;
    let mut bus = "bus0".to_string();
    let mut id = None;
    let mut direction = None;
    let mut kind = "";
    let mut data_length = None;
    let mut dlc = None;
    let mut data_start = None;
    for (position, column) in columns.iter().enumerate() {
        if column == "D" {
            data_start = Some(position);
            break;
        }
        let value = *words.get(position).ok_or("PCAN record has fewer fields than $COLUMNS")?;
        match column.as_str() {
            "N" | "R" => {}
            "O" => timestamp = Some(pcan_millis_to_us(value)?),
            "T" => {
                kind = value;
                if matches!(kind, "ER" | "ST" | "EC" | "EV") { return Ok(None); }
            }
            "B" => {
                let number = value.parse::<u8>().map_err(|_| "invalid PCAN bus number")?;
                if !(1..=16).contains(&number) { return Err("PCAN bus must be 1..16".into()); }
                bus = format!("bus{}", number - 1);
            }
            "I" => id = Some(pcan_id(value)?),
            "d" => direction = Some(pcan_direction(value)?),
            "l" => data_length = Some(value.parse::<usize>()
                .map_err(|_| "invalid PCAN data length")?),
            "L" => dlc = Some(value.parse::<usize>()
                .map_err(|_| "invalid PCAN DLC")?),
            other => return Err(format!("unsupported PCAN column '{other}'")),
        }
    }
    let fd_flags = match kind {
        "DT" | "RR" => FdFlags::default(),
        "FD" | "FB" | "FE" | "BI" => FdFlags {
            bitrate_switch: matches!(kind, "FB" | "BI"),
            error_state_indicator: matches!(kind, "FE" | "BI"),
        },
        _ => return Err(format!("unsupported PCAN 2.x message type '{kind}'")),
    };
    let is_fd = matches!(kind, "FD" | "FB" | "FE" | "BI");
    let wire_len = dlc.map(|n| if is_fd { pcan_fd_dlc_bytes(n) }
        else if n <= 8 { Ok(n) } else { Err("PCAN classic CAN DLC exceeds 8".into()) })
        .transpose()?;
    if let (Some(actual), Some(expected)) = (data_length, wire_len) {
        if actual != expected {
            return Err(format!("PCAN data length {actual} conflicts with DLC ({expected} bytes)"));
        }
    }
    let length = data_length.or(wire_len).ok_or("PCAN data length missing")?;
    let start = data_start.ok_or("PCAN data column missing")?;
    let raw_data = &words[start..];
    let frame_kind = if kind == "RR" {
        if !raw_data.is_empty() { return Err("PCAN RTR must not contain payload bytes".into()); }
        let dlc: u8 = length.try_into().map_err(|_| "PCAN RTR length exceeds u8")?;
        FrameKind::RemoteRequest { dlc }
    } else {
        if raw_data.len() != length {
            return Err(format!("PCAN data length {length} does not match {} bytes", raw_data.len()));
        }
        FrameKind::Data
    };
    let payload = if matches!(frame_kind, FrameKind::Data) {
        raw_data.iter().map(|word| parse_hex_byte(word))
            .collect::<Result<Vec<_>, _>>()?
    } else { Vec::new() };
    pcan_frame(timestamp.ok_or("PCAN timestamp missing")?, bus,
        id.ok_or("PCAN CAN ID missing")?, direction.ok_or("PCAN direction missing")?,
        frame_kind, payload, fd_flags, is_fd)
}

fn parse_vector_asc_line(line: &str) -> Result<Option<CanFrame>, String> {
    let normalized = line.to_ascii_lowercase();
    if normalized.starts_with("date ") || normalized.starts_with("base ")
        || normalized.starts_with("internal events")
        || normalized.starts_with("begin triggerblock")
        || normalized.starts_with("end triggerblock")
        || line.starts_with("//") {
        return Ok(None);
    }
    let words: Vec<&str> = line.split_whitespace().collect();
    if words.is_empty() { return Ok(None); }
    // Vector logs also contain error/event records. Only data-frame-shaped
    // records are imported; CAN FD rows must use a dedicated validated parser.
    if !words[0].bytes().next().is_some_and(|byte| byte.is_ascii_digit()) {
        return Ok(None);
    }
    if words.len() > 1 && words[1].eq_ignore_ascii_case("CANFD") {
        return Err("Vector CAN FD ASC row is not yet supported".into());
    }
    if words.len() < 6 {
        if words.get(3).is_some_and(|v| v.eq_ignore_ascii_case("Rx") || v.eq_ignore_ascii_case("Tx")) {
            return Err("Vector ASC CAN record has fewer than six columns".into());
        }
        return Ok(None);
    }
    let direction = match words[3].to_ascii_lowercase().as_str() {
        "rx" => Direction::Received, "tx" => Direction::Transmitted,
        _ => return Ok(None), // Vector event record, not a CAN frame
    };
    let timestamp = parse_timestamp(words[0])?;
    let bus = words[1].parse::<u16>().map_err(|_| "invalid Vector ASC channel")?;
    let extended = words[2].ends_with('x') || words[2].ends_with('X');
    let raw = if extended { &words[2][..words[2].len()-1] } else { words[2] };
    let id = parse_hex_id(raw, extended)?;
    let len = words[5].parse::<u8>().map_err(|_| "invalid Vector ASC DLC")?;
    if len > 8 { return Err("classic Vector ASC DLC exceeds 8".into()); }
    let kind = match words[4].to_ascii_lowercase().as_str() {
        "d" => FrameKind::Data,
        "r" => FrameKind::RemoteRequest { dlc: len },
        _ => return Err("Vector ASC frame type must be d or r".into()),
    };
    let data = if matches!(kind, FrameKind::RemoteRequest { .. }) {
        Vec::new()
    } else {
        if words.len() < 6 + usize::from(len) {
            return Err("Vector ASC data shorter than DLC".into());
        }
        words[6..6+usize::from(len)].iter().map(|v| parse_hex_byte(v))
            .collect::<Result<Vec<_>, _>>()?
    };
    let mut result = CanFrame::new(Some(timestamp), format!("bus{bus}"), id, kind, data, false, FdFlags::default())
        .map_err(|err| err.to_string())?;
    result.direction = direction;
    Ok(Some(result))
}

fn parse_carbus_line(line: &str, version_2: bool) -> Result<Option<CanFrame>, String> {
    let values: Vec<&str> = line.split_whitespace().collect();
    if values.len() < 5 { return Err("CARBUS Analyzer record has fewer than five columns".into()); }
    let timestamp = values[0].replace(',', "").parse::<u64>()
        .map_err(|_| "invalid CARBUS timestamp")?;
    let timestamp = if version_2 {
        timestamp.checked_mul(1000).ok_or("CARBUS timestamp overflow")?
    } else { timestamp };
    let bus = values[1].parse::<u16>().map_err(|_| "invalid CARBUS channel")?;
    let id = parse_hex_id(values[3], false)?;
    let len = usize::from_str_radix(values[4], 16).map_err(|_| "invalid CARBUS length")?;
    if len > 8 { return Err("CARBUS classic data length exceeds 8".into()); }
    if values.len() < 5 + len { return Err("CARBUS payload shorter than length".into()); }
    let bytes = values[5..5+len].iter().map(|v| parse_hex_byte(v))
        .collect::<Result<Vec<_>, _>>()?;
    Ok(Some(frame(Some(timestamp), format!("bus{bus}"), id, bytes, Direction::Received)?))
}

fn parse_busmaster_line(line: &str) -> Result<Option<CanFrame>, String> {
    let words: Vec<&str> = line.split_whitespace().collect();
    if words.len() < 6 { return Err("BUSMASTER row has fewer than six columns".into()); }
    let clock: Vec<&str> = words[0].split(':').collect();
    if clock.len() != 4 { return Err("BUSMASTER time must be hh:mm:ss:ms".into()); }
    let h = clock[0].parse::<u64>().map_err(|_| "invalid BUSMASTER hour")?;
    let m = clock[1].parse::<u64>().map_err(|_| "invalid BUSMASTER minute")?;
    let sec = clock[2].parse::<u64>().map_err(|_| "invalid BUSMASTER second")?;
    let ms = clock[3].parse::<u64>().map_err(|_| "invalid BUSMASTER millisecond")?;
    if h > 23 || m > 59 || sec > 59 || ms > 999 {
        return Err("BUSMASTER clock component outside range".into());
    }
    let timestamp = ((h * 3600 + m * 60 + sec) * 1000 + ms) * 1000;
    let direction = match words[1].to_ascii_uppercase().as_str() {
        "RX" => Direction::Received,
        "TX" => Direction::Transmitted,
        _ => return Err("BUSMASTER direction must be Rx/Tx".into()),
    };
    let bus = words[2].parse::<u16>().map_err(|_| "invalid BUSMASTER channel")?;
    let frame_type = words[4].to_ascii_uppercase();
    let (extended, rtr) = match frame_type.as_str() {
        "S" => (false, false), "X" => (true, false),
        "SR" => (false, true), "XR" => (true, true),
        _ => return Err(format!("invalid BUSMASTER type '{}'", words[4])),
    };
    let id = parse_hex_id(words[3], extended)?;
    if !extended && id.is_extended() { return Err("standard BUSMASTER frame has extended ID".into()); }
    let len = words[5].parse::<u8>().map_err(|_| "invalid BUSMASTER DLC")?;
    if len > 8 { return Err("BUSMASTER classic DLC cannot exceed 8".into()); }
    let kind = if rtr { FrameKind::RemoteRequest { dlc: len } } else { FrameKind::Data };
    let data = if rtr { Vec::new() } else {
        if words.len() < 6 + usize::from(len) { return Err("BUSMASTER payload shorter than DLC".into()); }
        words[6..6+usize::from(len)].iter().map(|byte| parse_hex_byte(byte))
            .collect::<Result<Vec<_>, _>>()?
    };
    let mut frame = CanFrame::new(Some(timestamp), format!("bus{bus}"), id, kind, data, false, FdFlags::default())
        .map_err(|err| err.to_string())?;
    frame.direction = direction;
    Ok(Some(frame))
}

fn parse_microchip_line(line: &str) -> Result<Option<CanFrame>, String> {
    let values: Vec<&str> = line.split(';').map(str::trim).collect();
    if values.len() < 4 { return Err("Microchip row requires timestamp, direction, ID and length".into()); }
    let millis = values[0].parse::<u64>().map_err(|_| "invalid Microchip millisecond timestamp")?;
    let timestamp = millis.checked_mul(1000).ok_or("Microchip timestamp overflow")?;
    let direction = match values[1].to_ascii_uppercase().as_str() {
        "RX" => Direction::Received, "TX" => Direction::Transmitted,
        _ => return Err("Microchip direction must be RX/TX".into()),
    };
    let id = parse_hex_id(values[2], false)?;
    let len = values[3].parse::<usize>().map_err(|_| "invalid Microchip payload length")?;
    if len > 8 { return Err("Microchip classic payload exceeds eight bytes".into()); }
    if values.len() < 4 + len { return Err("Microchip data shorter than length".into()); }
    let data = values[4..4+len].iter().map(|value| parse_hex_byte(value))
        .collect::<Result<Vec<_>, _>>()?;
    Ok(Some(frame(Some(timestamp), "bus0".into(), id, data, direction)?))
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

/// Write compact Linux candump text, including CAN FD flags and remote requests.
/// Standard candump syntax does not encode Rx/Tx direction; that metadata is
/// intentionally omitted. All other CanFrame fields represented here round-trip.
pub fn write_candump<W: Write>(mut writer: W, frames: &[CanFrame]) -> Result<(), ExportError> {
    for (index, frame) in frames.iter().enumerate() {
        if frame.interface.chars().any(char::is_whitespace) || frame.interface.is_empty() {
            return Err(ExportError::Unsupported {
                index, reason: "candump interfaces cannot contain whitespace".into(),
            });
        }
    }
    for frame in frames {
        if let Some(us) = frame.timestamp_us {
            write!(writer, "({}.{:06}) ", us / 1_000_000, us % 1_000_000)?;
        }
        write!(writer, "{} {}#", frame.interface, frame.id)?;
        match frame.kind {
            FrameKind::RemoteRequest { dlc } => write!(writer, "R{dlc}")?,
            FrameKind::Data if frame.is_fd => {
                let flags = u8::from(frame.fd_flags.bitrate_switch)
                    | (u8::from(frame.fd_flags.error_state_indicator) << 1);
                write!(writer, "#{flags:X}")?;
                for byte in &frame.data { write!(writer, "{byte:02X}")?; }
            }
            FrameKind::Data => {
                for byte in &frame.data { write!(writer, "{byte:02X}")?; }
            }
        }
        writeln!(writer)?;
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
    fn busmaster_and_microchip_examples_load_without_losing_frames() {
        let native = load(include_str!("../../../examples/GVRET_Log.csv"));
        let busmaster = load(include_str!("../../../examples/BusMasterLog.log"));
        let microchip = load(include_str!("../../../examples/MicrochipLog.log"));
        assert_eq!(busmaster.format, TraceFormat::Busmaster);
        assert_eq!(microchip.format, TraceFormat::Microchip);
        assert_eq!(busmaster.frames.len(), native.frames.len());
        assert_eq!(microchip.frames.len(), native.frames.len());
        for (n, (b, m)) in busmaster.frames.iter().zip(&microchip.frames).enumerate() {
            let expected = &native.frames[n];
            assert_eq!(b.id, expected.id);
            assert_eq!(b.data, expected.data);
            assert_eq!(m.id, expected.id);
            assert_eq!(m.data, expected.data);
            assert_eq!(m.timestamp_us, expected.timestamp_us);
        }
        assert_eq!(busmaster.frames[0].timestamp_us, Some(80_414_992_000));
    }

    #[test]
    fn busmaster_rtr_and_errors() {
        let text = "***BUSMASTER Ver 3***\n00:00:00:020 Tx 1 00000123 sr 4\n";
        let parsed = load(text);
        assert_eq!(parsed.frames[0].kind, FrameKind::RemoteRequest { dlc: 4 });
        assert_eq!(parsed.frames[0].direction, Direction::Transmitted);
        assert_eq!(parsed.frames[0].interface, "bus1");
        assert!(read_trace(Cursor::new("***BUSMASTER Ver 3***\n00:00:00:020 Rx 0 123 s 2 11\n")).is_err());
    }


    #[test]
    fn carbus_analyzer_v2_cr_line_endings_and_us_timestamp() {
        let text = include_str!("../../../examples/CarBusAnalyzer.trc");
        let result = load(text);
        assert_eq!(result.format, TraceFormat::CarbusAnalyzer);
        assert!(result.frames.len() > 20);
        assert_eq!(result.frames[0].timestamp_us, Some(46_016_000));
        assert_eq!(result.frames[0].id, CanId::Standard(0x45a));
        assert_eq!(result.frames[0].data, vec![0x5A, 0x04, 0, 0, 0, 0, 0, 0]);
        let converted = text.replace('\n', "\r");
        assert_eq!(load(&converted).frames, result.frames);
        let v3 = load("@ TEXT @ 3 @ 0 @\n46,016 0 0004 7FF 1 AA\n");
        assert_eq!(v3.frames[0].timestamp_us, Some(46_016));
    }


    #[test]
    fn vector_asc_classic_rx_tx_and_remote() {
        let asc = "date Mon Oct 23 17:00:00 2026\n\
                   base hex timestamps absolute\n\
                   internal events logged\n\
                   Begin Triggerblock Mon Oct 23 17:00:00 2026\n\
                   0.001000 1 123 Rx d 2 AA BB\n\
                   0.001234 2 18FF50E5x Tx d 1 00\n\
                   0.001235 2 123 Rx r 8\n\
                   End Triggerblock\n";
        let trace = load(asc);
        assert_eq!(trace.format, TraceFormat::VectorAsc);
        assert_eq!(trace.frames.len(), 3);
        assert_eq!(trace.frames[0].timestamp_us, Some(1000));
        assert_eq!(trace.frames[0].id, CanId::Standard(0x123));
        assert_eq!(trace.frames[0].data, vec![0xAA,0xBB]);
        assert_eq!(trace.frames[1].id, CanId::Extended(0x18ff50e5));
        assert_eq!(trace.frames[1].direction, Direction::Transmitted);
        assert_eq!(trace.frames[2].kind, FrameKind::RemoteRequest { dlc: 8 });
        assert_eq!(trace.frames[2].data.len(), 0);
    }

    #[test]
    fn vector_fd_rows_fail_explicitly() {
        let text = "date Mon Oct 23 2026\n1.000 CANFD 1 Rx 123 0 0 8 8 00 11\n";
        let error = read_trace(Cursor::new(text)).unwrap_err();
        assert!(error.to_string().contains("line 2"));
        assert!(error.to_string().contains("not yet supported"));
    }


    #[test]
    fn candump_export_round_trip_classic_fd_rtr_and_timestamp() {
        let standard = CanFrame::new(Some(1_234_567), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![0xAA, 0xBB], false, FdFlags::default()).unwrap();
        let extended_low = CanFrame::new(None, "can1", CanId::Extended(0x123),
            FrameKind::Data, vec![0xDE, 0xAD], true,
            FdFlags { bitrate_switch: true, error_state_indicator: true }).unwrap();
        let remote = CanFrame::new(Some(10), "vcan0", CanId::Standard(0x500),
            FrameKind::RemoteRequest { dlc: 8 }, vec![], false, FdFlags::default()).unwrap();
        let originals = vec![standard, extended_low, remote];
        let mut out = Vec::new();
        write_candump(&mut out, &originals).unwrap();
        assert_eq!(read_trace(Cursor::new(out)).unwrap().frames, originals);
    }

    #[test]
    fn candump_export_direction_is_not_represented() {
        let mut sent = CanFrame::new(Some(20), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![1], false, FdFlags::default()).unwrap();
        sent.direction = Direction::Transmitted;
        let mut out = Vec::new();
        write_candump(&mut out, &[sent]).unwrap();
        let parsed = read_trace(Cursor::new(out)).unwrap().frames.remove(0);
        assert_eq!(parsed.direction, Direction::Unknown);
        assert_eq!(parsed.timestamp_us, Some(20));
    }

    #[test]
    fn pcan_legacy_versions_preserve_bus_timestamp_id_and_rtr() {
        let old = ";$FILEVERSION=1.1\n;$STARTTIME=37704.5364870833\n\
            1) 1059.9 Rx 0300 2 AA BB\n\
            2) 1352.7 Tx 0100 3 RTR\n";
        let one = load(old);
        assert_eq!(one.format, TraceFormat::PcanTrc);
        assert_eq!(one.frames.len(), 2);
        assert_eq!(one.frames[0].timestamp_us, Some(1_059_900));
        assert_eq!(one.frames[0].interface, "bus0");
        assert_eq!(one.frames[0].data, vec![0xAA, 0xBB]);
        assert_eq!(one.frames[1].direction, Direction::Transmitted);
        assert_eq!(one.frames[1].kind, FrameKind::RemoteRequest { dlc: 3 });

        let two = load(";$FILEVERSION=1.2\n1) 1298.945 2 Tx 00000400 2 00 00\n");
        assert_eq!(two.frames[0].timestamp_us, Some(1_298_945));
        assert_eq!(two.frames[0].interface, "bus1");
        assert_eq!(two.frames[0].id, CanId::Extended(0x400));

        let three = load(";$FILEVERSION=1.3\n1) 1298.945 2 Rx 0400 - 2 00 01\n");
        assert_eq!(three.frames[0].interface, "bus1");
        assert_eq!(three.frames[0].id, CanId::Standard(0x400));
    }

    #[test]
    fn pcan_2_x_supports_can_fd_brs_esi_and_remote_requests() {
        let v20 = ";$FILEVERSION=2.0\n;$COLUMNS=N,O,T,I,d,l,D\n\
             1 1334.416 FB 00000500 Tx 12 01 02 03 04 05 06 07 08 09 0A 0B 0C\n\
             2 1336.543 RR 0100 Rx 3\n";
        let parsed = load(v20);
        assert_eq!(parsed.frames.len(), 2);
        assert_eq!(parsed.frames[0].timestamp_us, Some(1_334_416));
        assert_eq!(parsed.frames[0].id, CanId::Extended(0x500));
        assert_eq!(parsed.frames[0].direction, Direction::Transmitted);
        assert!(parsed.frames[0].is_fd);
        assert!(parsed.frames[0].fd_flags.bitrate_switch);
        assert!(!parsed.frames[0].fd_flags.error_state_indicator);
        assert_eq!(parsed.frames[0].data.len(), 12);
        assert_eq!(parsed.frames[1].kind, FrameKind::RemoteRequest { dlc: 3 });

        let v21 = ";$FILEVERSION=2.1\n;$COLUMNS=N,O,T,B,I,d,R,L,D\n\
             1 1334.416 BI 2 0500 Rx - 9 01 02 03 04 05 06 07 08 09 0A 0B 0C\n\
             2 1334.522 ER 2 - Rx - 5 04 00 02 00 00\n";
        let parsed = load(v21);
        assert_eq!(parsed.frames.len(), 1);
        assert_eq!(parsed.frames[0].interface, "bus1");
        assert_eq!(parsed.frames[0].id, CanId::Standard(0x500));
        assert_eq!(parsed.frames[0].data.len(), 12);
        assert!(parsed.frames[0].fd_flags.bitrate_switch);
        assert!(parsed.frames[0].fd_flags.error_state_indicator);
    }

    #[test]
    fn pcan_invalid_versions_and_length_mismatches_fail_explicitly() {
        let bad_version = read_trace(Cursor::new(";$FILEVERSION=3.0\n")).unwrap_err();
        assert!(bad_version.to_string().contains("unsupported PCAN TRC version"));
        let bad_dlc = read_trace(Cursor::new(
            ";$FILEVERSION=2.1\n;$COLUMNS=N,O,T,B,I,d,R,L,D\n\
              1 1334.416 FD 1 0500 Rx - 9 01 02\n"
        )).unwrap_err();
        assert!(bad_dlc.to_string().contains("length 12"));
    }

    #[test]
    fn ixxat_minilog_csv_matches_savvycan_writer_and_distinguishes_id_width() {
        let input = "ASCII Trace IXXAT SavvyCAN V1\n\
            Date: 23:10:2026\nStart time: 0:0:0\nStop time: 0:0:2\n\
            Overruns: 0\nBaudrate: 500 kbit/s\n\
            \"Time\",\"Identifier (hex)\",\"Format\",\"Flags\",\"Data (hex)\"\n\
            \"0:0:1.250\",\"00000123\",\"Std\",\"\",\"AA 11 \"\n\
            \"0:0:2.000\",\"00000123\",\"Ext\",\"\",\"01 \"\n";
        let trace = load(input);
        assert_eq!(trace.format, TraceFormat::IxxatMinilog);
        assert_eq!(trace.frames.len(), 2);
        assert_eq!(trace.frames[0].timestamp_us, Some(1_250_000));
        assert_eq!(trace.frames[0].id, CanId::Standard(0x123));
        assert_eq!(trace.frames[0].data, vec![0xaa, 0x11]);
        assert_eq!(trace.frames[0].direction, Direction::Unknown);
        assert_eq!(trace.frames[1].id, CanId::Extended(0x123));
    }

    #[test]
    fn ixxat_bad_flags_and_payload_are_not_discarded_silently() {
        let prefix = "ASCII Trace IXXAT SavvyCAN V1\n";
        let flags = format!("{prefix}\"0:0:2.000\",\"00000123\",\"Std\",\"ERR\",\"01\"\n");
        assert!(read_trace(Cursor::new(flags)).unwrap_err().to_string().contains("flags"));
        let bytes = format!("{prefix}\"0:0:2.000\",\"00000123\",\"Std\",\"\",\"GG\"\n");
        assert!(read_trace(Cursor::new(bytes)).unwrap_err().to_string().contains("invalid"));
    }

    #[test]
    fn vehicle_spy_csv_preserves_timestamp_tx_rx_id_width_and_quoted_commas() {
        let input = "Line,Abs Time(Sec),Rel Time (Sec),Status,Er,Tx,Description,Network,Node,Arb ID,Remote,Xtd,B1,B2,B3,B4,B5,B6,B7,B8,Value,Trigger,Signals\n\
            Line,Abs Time(Sec),Rel Time (Sec),Status,Er,Tx,Description,Network,Node,Arb ID,Remote,Xtd,B1,B2,B3,B4,B5,B6,B7,B8,Value,Trigger,Signals\n\
            2,2550.368293675,0.003818,67371008,F,F,\"HS CAN, ECU\",HS CAN,,119,F,F,00,00,00,00,00,00,0D,8B,,,\n\
            3,2551.000000000,0.003818,67371008,F,T,HS CAN,HS CAN,,119,F,T,11,,,,,,,,,,,\n";
        let trace = load(input);
        assert_eq!(trace.format, TraceFormat::VehicleSpy);
        assert_eq!(trace.frames.len(), 2);
        assert_eq!(trace.frames[0].timestamp_us, Some(2_550_368_293));
        assert_eq!(trace.frames[0].id, CanId::Standard(0x119));
        assert_eq!(trace.frames[0].data, vec![0, 0, 0, 0, 0, 0, 0x0D, 0x8B]);
        assert_eq!(trace.frames[0].direction, Direction::Received);
        assert_eq!(trace.frames[1].id, CanId::Extended(0x119));
        assert_eq!(trace.frames[1].direction, Direction::Transmitted);
        assert_eq!(trace.frames[1].data, vec![0x11]);
    }

    #[test]
    fn vehicle_spy_invalid_data_and_missing_columns_fail() {
        let header = "Line,Abs Time(Sec),B1\n";
        let missing = format!("{header}1,1.000000,0,0,F,F,a,b,,119,F,F,11\n");
        assert!(read_trace(Cursor::new(missing)).is_err());
        let bad_quote = format!("{header}1,1.000000,0,0,F,F,\"broken,b,,119,F,F,11,,,,,,,,,,,\n");
        assert!(read_trace(Cursor::new(bad_quote)).is_err());
    }

    #[test]
    fn cabana_csv_preserves_decimal_ids_bus_bytes_and_microseconds() {
        let text = "time,addr,bus,data\n123.456789123,291,0,AABBCC\n\
            124.000001,419385573,1,0102030405060708\n";
        let trace = load(text);
        assert_eq!(trace.format, TraceFormat::CabanaCsv);
        assert_eq!(trace.frames.len(), 2);
        assert_eq!(trace.frames[0].timestamp_us, Some(123_456_789));
        assert_eq!(trace.frames[0].id, CanId::Standard(291));
        assert_eq!(trace.frames[0].data, vec![0xaa, 0xbb, 0xcc]);
        assert_eq!(trace.frames[0].direction, Direction::Unknown);
        assert_eq!(trace.frames[1].id, CanId::Extended(419385573));
        assert_eq!(trace.frames[1].interface, "bus1");
    }

    #[test]
    fn cabana_rejects_odd_hex_and_unmarked_can_fd() {
        let input = "time,addr,bus,data\n1.0,291,0,AAB\n";
        assert!(read_trace(Cursor::new(input)).unwrap_err().to_string().contains("pairs"));
        let input = "time,addr,bus,data\n1.0,291,0,000102030405060708\n";
        assert!(read_trace(Cursor::new(input)).unwrap_err().to_string().contains("CAN FD"));
    }

    #[test]
    fn candump_remains_auto_detected() {
        let frames = load("(12.123456) can0 123#AABB\n");
        assert_eq!(frames.format, TraceFormat::Candump);
        assert_eq!(frames.frames[0].timestamp_us, Some(12_123_456));
    }
}
