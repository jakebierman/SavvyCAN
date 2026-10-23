//! Deterministic, transport-independent CAN traffic statistics by interface
//! and exact 11/29-bit CAN identifier.
use crate::{CanFrame, CanId, FrameKind};
use std::collections::HashMap;
use std::io::{self, Write};

#[derive(Debug, Clone, PartialEq)]
pub struct IdStatistics {
    pub interface: String,
    pub id: CanId,
    pub count: usize,
    pub classic_count: usize,
    pub fd_count: usize,
    pub remote_count: usize,
    pub payload_bytes: usize,
    pub first_us: Option<u64>,
    pub last_us: Option<u64>,
    pub min_period_us: Option<u64>,
    pub max_period_us: Option<u64>,
    pub average_period_us: Option<f64>,
    pub estimated_hz: Option<f64>,
}

#[derive(Default)]
struct Accum {
    count: usize,
    classic: usize,
    fd: usize,
    remote: usize,
    bytes: usize,
    timestamps: Vec<u64>,
}

/// All timestamped occurrences are sorted by time before interval calculation,
/// even when a source file contains out-of-order frames. Missing timestamps
/// do not generate invented periods or rates.
pub fn analyze_ids(frames: &[CanFrame]) -> Vec<IdStatistics> {
    let mut grouped: HashMap<(String, CanId), Accum> = HashMap::new();
    for frame in frames {
        let group = grouped.entry((frame.interface.clone(), frame.id)).or_default();
        group.count += 1;
        if frame.is_fd { group.fd += 1; } else { group.classic += 1; }
        if matches!(frame.kind, FrameKind::RemoteRequest { .. }) { group.remote += 1; }
        group.bytes = group.bytes.saturating_add(frame.data.len());
        if let Some(us) = frame.timestamp_us { group.timestamps.push(us); }
    }
    let mut result: Vec<IdStatistics> = grouped.into_iter().map(|((interface, id), mut group)| {
        group.timestamps.sort_unstable();
        let first = group.timestamps.first().copied();
        let last = group.timestamps.last().copied();
        let mut intervals = group.timestamps.windows(2).map(|pair| pair[1] - pair[0]);
        let first_interval = intervals.next();
        let (mut min, mut max) = (first_interval, first_interval);
        for period in intervals {
            min = min.map(|current| current.min(period));
            max = max.map(|current| current.max(period));
        }
        let average = if group.timestamps.len() >= 2 {
            let elapsed = (last.unwrap() - first.unwrap()) as f64;
            Some(elapsed / (group.timestamps.len() - 1) as f64)
        } else { None };
        let hz = average.filter(|value| *value > 0.0)
            .map(|period| 1_000_000.0 / period);
        IdStatistics {
            interface, id, count: group.count,
            classic_count: group.classic, fd_count: group.fd,
            remote_count: group.remote, payload_bytes: group.bytes,
            first_us: first, last_us: last, min_period_us: min,
            max_period_us: max, average_period_us: average, estimated_hz: hz,
        }
    }).collect();
    result.sort_by(|a, b| a.interface.cmp(&b.interface)
        .then_with(|| a.id.value().cmp(&b.id.value()))
        .then_with(|| a.id.is_extended().cmp(&b.id.is_extended())));
    result
}

fn csv_text(raw: &str) -> String {
    let guarded = if raw.trim_start().chars().next().is_some_and(
        |c| matches!(c, '=' | '+' | '-' | '@')
    ) { format!("'{raw}") } else { raw.to_owned() };
    format!("\"{}\"", guarded.replace('"', "\"\""))
}

fn optional<T: std::fmt::Display>(value: Option<T>) -> String {
    value.map(|number| number.to_string()).unwrap_or_default()
}

/// Export a machine-readable ID summary; one row per interface/ID pair.
pub fn write_id_stats_csv<W: Write>(
    mut writer: W, frames: &[CanFrame],
) -> Result<usize, io::Error> {
    let groups = analyze_ids(frames);
    writeln!(writer,
        "interface,id,extended,count,classic,fd,rtr,payload_bytes,first_us,last_us,min_period_us,max_period_us,average_period_us,estimated_hz"
    )?;
    for row in &groups {
        writeln!(writer, "{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
            csv_text(&row.interface), csv_text(&row.id.to_string()),
            row.id.is_extended(), row.count, row.classic_count, row.fd_count,
            row.remote_count, row.payload_bytes,
            optional(row.first_us), optional(row.last_us),
            optional(row.min_period_us), optional(row.max_period_us),
            optional(row.average_period_us), optional(row.estimated_hz),
        )?;
    }
    Ok(groups.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{FdFlags, FrameKind};

    fn frame(us: Option<u64>, id: CanId, data: &[u8]) -> CanFrame {
        CanFrame::new(us, "can0", id, FrameKind::Data, data.to_vec(),
            false, FdFlags::default()).unwrap()
    }

    #[test]
    fn per_id_frequency_and_intervals_use_sorted_timestamps() {
        let frames = vec![
            frame(Some(200_000), CanId::Standard(0x123), &[1, 2]),
            frame(Some(0), CanId::Standard(0x123), &[3, 4]),
            frame(Some(100_000), CanId::Standard(0x123), &[5, 6]),
            frame(None, CanId::Extended(0x123), &[9]),
        ];
        let stats = analyze_ids(&frames);
        assert_eq!(stats.len(), 2);
        let standard = &stats[0];
        assert_eq!(standard.id, CanId::Standard(0x123));
        assert_eq!(standard.count, 3);
        assert_eq!(standard.payload_bytes, 6);
        assert_eq!(standard.first_us, Some(0));
        assert_eq!(standard.last_us, Some(200_000));
        assert_eq!(standard.min_period_us, Some(100_000));
        assert_eq!(standard.max_period_us, Some(100_000));
        assert_eq!(standard.average_period_us, Some(100_000.0));
        assert_eq!(standard.estimated_hz, Some(10.0));
        assert_eq!(stats[1].id, CanId::Extended(0x123));
        assert_eq!(stats[1].estimated_hz, None);
    }

    #[test]
    fn csv_writer_marks_unknown_rates_and_escapes_interface_text() {
        let frame = CanFrame::new(None, "=can0", CanId::Standard(0x123),
            FrameKind::RemoteRequest { dlc: 8 }, Vec::new(),
            false, FdFlags::default()).unwrap();
        let mut out = Vec::new();
        assert_eq!(write_id_stats_csv(&mut out, &[frame]).unwrap(), 1);
        let output = String::from_utf8(out).unwrap();
        assert!(output.contains("\"'=can0\",\"123\",false,1,1,0,1,0,"));
        assert!(output.contains("estimated_hz"));
    }
}
