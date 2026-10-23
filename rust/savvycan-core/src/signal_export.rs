//! Safe, read-only export of every decoded DBC signal for external analysis.
//! Each signal gets its own record; unmatched frames and RTRs have no rows.
use crate::{CanFrame, DbcStack, DecodeError};
use std::fmt;
use std::io::{self, Write};

#[derive(Debug)]
pub enum SignalExportError {
    Io(io::Error),
    Decode { frame: usize, source: DecodeError },
}
impl fmt::Display for SignalExportError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(e) => e.fmt(f),
            Self::Decode { frame, source } => write!(f, "frame {frame}: {source}"),
        }
    }
}
impl std::error::Error for SignalExportError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(e) => Some(e),
            Self::Decode { source, .. } => Some(source),
        }
    }
}
impl From<io::Error> for SignalExportError {
    fn from(e: io::Error) -> Self { Self::Io(e) }
}

/// Escape CSV syntax and prevent spreadsheet formula interpretation in
/// DBC-controlled text cells. Numeric measurement columns remain numeric.
fn cell(input: &str) -> String {
    let guarded = if input.trim_start().chars().next().is_some_and(
        |c| matches!(c, '=' | '+' | '-' | '@')
    ) {
        format!("'{input}")
    } else {
        input.to_owned()
    };
    format!("\"{}\"", guarded.replace('"', "\"\""))
}

/// Export normalized signal rows. This is intentionally a different file
/// shape from GVRET CSV and requires an explicitly loaded DBC registry.
/// The count returned is number of exported signals (not frame count).
pub fn write_decoded_csv<W: Write>(
    mut writer: W,
    frames: &[CanFrame],
    dbc: &DbcStack,
) -> Result<usize, SignalExportError> {
    writeln!(writer,
        "timestamp_us,interface,can_id,extended,message,signal,raw,physical_value,unit,label"
    )?;
    let mut records = 0;
    for (index, frame) in frames.iter().enumerate() {
        let decoded = dbc.decode(frame).map_err(|source|
            SignalExportError::Decode { frame: index + 1, source }
        )?;
        let Some(message) = decoded else { continue; };
        let timestamp = frame.timestamp_us.map(|n| n.to_string()).unwrap_or_default();
        for signal in message.signals {
            writeln!(writer, "{},{},{},{},{},{},{},{},{},{}",
                timestamp,
                cell(&frame.interface),
                cell(&frame.id.to_string()),
                frame.id.is_extended(),
                cell(&message.name),
                cell(&signal.name),
                signal.raw,
                signal.value,
                cell(&signal.unit),
                cell(signal.text.as_deref().unwrap_or("")),
            )?;
            records += 1;
        }
    }
    Ok(records)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CanId, DbcDatabase, Direction, FdFlags, FrameKind};

    #[test]
    fn decoded_signal_csv_exports_scaled_values_and_escapes_labels() {
        let database = DbcDatabase::parse(
            "BO_ 291 Engine: 2 ECU\n\
             SG_ Voltage : 0|8@1+ (0.5,0) [0|255] \"V\" ECU\n\
             SG_ State : 8|8@1+ (1,0) [0|255] \"\" ECU\n\
             VAL_ 291 State 1 \"=1+1,Oops\" ;\n"
        ).unwrap();
        let mut registry = DbcStack::new();
        registry.add_global(database);
        let mut frame = CanFrame::new(Some(1234), "can0", CanId::Standard(291),
            FrameKind::Data, vec![32, 1], false, FdFlags::default()).unwrap();
        frame.direction = Direction::Received;
        let mut csv = Vec::new();
        assert_eq!(write_decoded_csv(&mut csv, &[frame], &registry).unwrap(), 2);
        let csv = String::from_utf8(csv).unwrap();
        assert!(csv.contains("timestamp_us,interface,can_id,extended"));
        assert!(csv.contains("1234,\"can0\",\"123\",false,\"Engine\",\"Voltage\",32,16,\"V\",\"\""));
        assert!(csv.contains("1234,\"can0\",\"123\",false,\"Engine\",\"State\",1,1,\"\",\"'=1+1,Oops\""));
    }

    #[test]
    fn undecoded_frames_are_ignored_and_errors_keep_frame_index() {
        let mut registry = DbcStack::new();
        registry.add_global(DbcDatabase::parse(
            "BO_ 291 Engine: 2 ECU\n SG_ Voltage : 8|8@1+ (1,0) [0|255] \"V\" ECU\n"
        ).unwrap());
        let frame = CanFrame::new(None, "can0", CanId::Standard(291),
            FrameKind::Data, vec![12], false, FdFlags::default()).unwrap();
        let other = CanFrame::new(None, "can0", CanId::Standard(292),
            FrameKind::Data, vec![12], false, FdFlags::default()).unwrap();
        let mut csv = Vec::new();
        let error = write_decoded_csv(&mut csv, &[other, frame], &registry).unwrap_err();
        assert!(error.to_string().contains("frame 2"));
    }
}
