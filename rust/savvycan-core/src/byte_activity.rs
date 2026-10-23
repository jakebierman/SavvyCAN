//! Per-byte data activity for reverse-engineering CAN payloads.
//! Statistics compare consecutive data frames in capture order within each
//! exact interface/11-bit-or-29-bit ID group. RTR frames never carry data.
use std::collections::HashMap;
use std::io::{self, Write};

use crate::{CanFrame, CanId, FrameKind, TraceFilter};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ByteActivity {
    pub interface: String,
    pub id: CanId,
    pub byte_index: usize,
    pub samples: usize,
    pub distinct_values: usize,
    pub changes: usize,
    pub bit_flips: usize,
    pub last_value: u8,
}

#[derive(Clone, Default)]
struct ByteAccum {
    samples: usize,
    seen: [u64; 4],
    changes: usize,
    bit_flips: usize,
    last: u8,
}

struct Group {
    byte: [ByteAccum; 64],
    previous: Option<Vec<u8>>,
}
impl Default for Group {
    fn default() -> Self {
        Self { byte: std::array::from_fn(|_| ByteAccum::default()), previous: None }
    }
}

/// Analyze borrowed frames without duplicating the capture in memory.
fn analyze_from<'a>(frames: impl Iterator<Item = &'a CanFrame>) -> Vec<ByteActivity> {
    let mut groups: HashMap<(String, CanId), Group> = HashMap::new();
    for frame in frames {
        if !matches!(frame.kind, FrameKind::Data) { continue; }
        let group = groups.entry((frame.interface.clone(), frame.id)).or_default();
        for (index, &value) in frame.data.iter().enumerate() {
            let sample = &mut group.byte[index];
            sample.samples += 1;
            sample.seen[(value / 64) as usize] |= 1u64 << (value % 64);
            if let Some(&previous) = group.previous.as_ref().and_then(|p| p.get(index)) {
                if previous != value {
                    sample.changes += 1;
                    sample.bit_flips += (previous ^ value).count_ones() as usize;
                }
            }
            sample.last = value;
        }
        // An absent position does not imply a value of zero; a subsequent
        // frame will not compare against a byte that wasn't present.
        group.previous = Some(frame.data.clone());
    }
    let mut result = Vec::new();
    for ((interface, id), group) in groups {
        for (byte_index, sample) in group.byte.into_iter().enumerate() {
            if sample.samples == 0 { continue; }
            result.push(ByteActivity {
                interface: interface.clone(), id, byte_index,
                samples: sample.samples,
                distinct_values: sample.seen.iter().map(|bits| bits.count_ones() as usize).sum(),
                changes: sample.changes, bit_flips: sample.bit_flips,
                last_value: sample.last,
            });
        }
    }
    result.sort_by(|a, b| a.interface.cmp(&b.interface)
        .then_with(|| a.id.value().cmp(&b.id.value()))
        .then_with(|| a.id.is_extended().cmp(&b.id.is_extended()))
        .then_with(|| a.byte_index.cmp(&b.byte_index)));
    result
}

pub fn analyze_byte_activity(frames: &[CanFrame]) -> Vec<ByteActivity> {
    analyze_from(frames.iter())
}

/// Apply the same non-destructive query as the trace viewer, without cloning
/// large CAN payload buffers. A matching RTR frame still supplies no bytes.
pub fn analyze_byte_activity_matching(
    frames: &[CanFrame], filter: &TraceFilter,
) -> Vec<ByteActivity> {
    analyze_from(frames.iter().filter(|frame| filter.matches(frame)))
}

fn csv_field(raw: &str) -> String {
    let safe = if raw.trim_start().chars().next().is_some_and(
        |c| matches!(c, '=' | '+' | '-' | '@')
    ) { format!("'{raw}") } else { raw.to_owned() };
    format!("\"{}\"", safe.replace('"', "\"\""))
}

/// Write one CSV row per observed byte index per interface/ID; the number
/// of returned rows is independent of the number of input CAN frames.
pub fn write_byte_activity_csv<W: Write>(
    mut writer: W, frames: &[CanFrame],
) -> Result<usize, io::Error> {
    write_byte_activity_rows(&mut writer, analyze_byte_activity(frames))
}

/// Export selected interface/CAN-ID/time/payload records without cloning all
/// matching frames. A header is written even when nothing matches.
pub fn write_byte_activity_csv_matching<W: Write>(
    mut writer: W, frames: &[CanFrame], filter: &TraceFilter,
) -> Result<usize, io::Error> {
    write_byte_activity_rows(&mut writer, analyze_byte_activity_matching(frames, filter))
}

fn write_byte_activity_rows<W: Write>(
    mut writer: W, rows: Vec<ByteActivity>,
) -> Result<usize, io::Error> {
    writeln!(writer,
        "interface,id,extended,byte_index,samples,distinct_values,changes,bit_flips,last_value_hex"
    )?;
    for row in &rows {
        writeln!(writer, "{},{},{},{},{},{},{},{},{:02X}",
            csv_field(&row.interface), csv_field(&row.id.to_string()),
            row.id.is_extended(), row.byte_index, row.samples,
            row.distinct_values, row.changes, row.bit_flips, row.last_value
        )?;
    }
    Ok(rows.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{FdFlags, FrameKind};

    fn frame(interface: &str, id: CanId, payload: Vec<u8>, fd: bool) -> CanFrame {
        CanFrame::new(None, interface, id, FrameKind::Data,
            payload, fd, FdFlags::default()).unwrap()
    }

    #[test]
    fn counts_byte_changes_and_bit_flips_without_inventing_missing_values() {
        let id = CanId::Standard(0x123);
        let frames = vec![
            frame("can0", id, vec![0, 0xFF], false),
            frame("can0", id, vec![1, 0xFF], false),
            frame("can0", id, vec![3], false),
            frame("can0", id, vec![3, 0x00], false),
        ];
        let rows = analyze_byte_activity(&frames);
        assert_eq!(rows.len(), 2);
        assert_eq!((rows[0].samples, rows[0].distinct_values,
            rows[0].changes, rows[0].bit_flips, rows[0].last_value),
            (4, 3, 2, 2, 3));
        assert_eq!((rows[1].samples, rows[1].distinct_values,
            rows[1].changes, rows[1].bit_flips, rows[1].last_value),
            (3, 2, 0, 0, 0));
    }

    #[test]
    fn separates_interfaces_and_id_width_and_ignores_rtr() {
        let s = CanId::Standard(0x123);
        let x = CanId::Extended(0x123);
        let mut frames = vec![
            frame("can0", s, vec![1], false),
            frame("can0", x, vec![2], false),
            frame("can1", s, vec![3], false),
        ];
        frames.push(CanFrame::new(None, "can0", s,
            FrameKind::RemoteRequest { dlc: 8 }, vec![], false,
            FdFlags::default()).unwrap());
        let rows = analyze_byte_activity(&frames);
        assert_eq!(rows.len(), 3);
        assert!(rows.iter().all(|row| row.samples == 1));
    }

    #[test]
    fn borrowed_filter_and_csv_match_the_full_capture_equivalent() {
        let frames = vec![
            frame("can0", CanId::Standard(0x123), vec![0, 1], false),
            frame("can0", CanId::Standard(0x456), vec![2, 3], false),
            frame("can0", CanId::Standard(0x123), vec![1, 1], false),
        ];
        let query = TraceFilter::parse("", "S:123").unwrap();
        let matching = analyze_byte_activity_matching(&frames, &query);
        assert_eq!(matching.len(), 2);
        assert_eq!(matching[0].changes, 1);
        let mut out = Vec::new();
        let count = write_byte_activity_csv_matching(&mut out, &frames, &query).unwrap();
        assert_eq!(count, 2);
        assert!(!String::from_utf8(out).unwrap().contains("456"));
    }

    #[test]
    fn full_can_fd_length_and_csv_output() {
        let mut bytes = vec![0; 64];
        bytes[63] = 0xA5;
        let rows = analyze_byte_activity(&[frame(
            "=bad", CanId::Extended(0x18FF1234), bytes, true)]);
        assert_eq!(rows.len(), 64);
        assert_eq!(rows[63].last_value, 0xA5);
        let mut out = Vec::new();
        let written = write_byte_activity_csv(&mut out, &[frame(
            "=bad", CanId::Extended(0x18FF1234), vec![0xA5], true)]).unwrap();
        assert_eq!(written, 1);
        let csv = String::from_utf8(out).unwrap();
        assert!(csv.contains("last_value_hex"));
        assert!(csv.contains("\"'=bad\""));
        assert!(csv.ends_with(",A5\n"));
    }
}
