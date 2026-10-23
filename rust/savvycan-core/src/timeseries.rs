//! Transport-independent numeric DBC signal series and scalable plot paths.
//! These routines never query ECUs or open a CAN interface.
use std::fmt::Write as _;

use crate::{CanFrame, DbcStack, FrameKind, TraceFilter};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HorizontalAxis {
    TimestampMicroseconds,
    FrameIndex,
}

#[derive(Debug, Clone, PartialEq)]
pub struct NumericSeries {
    pub signal: String,
    pub unit: String,
    pub axis: HorizontalAxis,
    pub points: Vec<(u64, f64)>,
    pub decode_errors: usize,
}

/// Extract one named signal from all matching data frames in input order.
/// Uses frame indices when any usable observation has an unknown timestamp;
/// it never combines timestamp and frame-index units on one axis.
pub fn extract_numeric_series(
    frames: &[CanFrame], stack: &DbcStack, filter: &TraceFilter, name: &str,
) -> Result<NumericSeries, String> {
    let name = name.trim();
    if name.is_empty() { return Err("Enter a DBC signal name to plot.".into()); }
    let mut observations = Vec::new();
    let mut unit = String::new();
    let mut decode_errors = 0;
    for (index, frame) in frames.iter().enumerate() {
        if !matches!(frame.kind, FrameKind::Data) || !filter.matches(frame) { continue; }
        match stack.decode(frame) {
            Ok(Some(decoded)) => {
                if let Some(signal) = decoded.signals.iter().find(|signal| signal.name == name) {
                    if signal.value.is_finite() {
                        if unit.is_empty() { unit = signal.unit.clone(); }
                        observations.push((index, frame.timestamp_us, signal.value));
                    }
                }
            }
            Ok(None) => {}
            Err(_) => decode_errors += 1,
        }
    }
    if observations.is_empty() {
        return Err(format!("No finite samples for DBC signal '{name}' in the selected trace."));
    }
    let timestamps = observations.iter().all(|(_, time, _)| time.is_some());
    let axis = if timestamps { HorizontalAxis::TimestampMicroseconds }
        else { HorizontalAxis::FrameIndex };
    let points = observations.into_iter().map(|(index, time, value)| (
        if timestamps { time.expect("validated above") } else { index as u64 }, value
    )).collect();
    Ok(NumericSeries { signal: name.into(), unit, axis, points, decode_errors })
}

#[derive(Debug, Clone, PartialEq)]
pub struct PlotPath {
    pub commands: String,
    pub source_points: usize,
    pub plotted_points: usize,
    pub first_x: u64,
    pub last_x: u64,
    pub min_y: f64,
    pub max_y: f64,
}

/// Build an SVG polyline in a fixed 1000-by-300 logical coordinate system.
/// Large series use min/max buckets so narrow spikes survive downsampling.
pub fn plot_path(points: &[(u64, f64)], max_points: usize) -> Result<PlotPath, String> {
    let mut valid: Vec<(u64, f64)> = points.iter().copied()
        .filter(|(_, value)| value.is_finite()).collect();
    if valid.is_empty() { return Err("No finite numeric samples to plot.".into()); }
    valid.sort_by_key(|point| point.0);
    let first = valid.first().expect("nonempty").0;
    let last = valid.last().expect("nonempty").0;
    let minimum = valid.iter().map(|point| point.1).fold(f64::INFINITY, f64::min);
    let maximum = valid.iter().map(|point| point.1).fold(f64::NEG_INFINITY, f64::max);
    render_path(valid, max_points, first, last, minimum, maximum)
}

/// Plot samples against *explicit shared axes*. Unlike `plot_path`, this
/// keeps different DBC signals aligned to exactly the same X coordinates.
/// The X interval is inclusive, and points outside it never enter the path.
pub fn plot_path_on_axes(
    points: &[(u64, f64)], max_points: usize,
    x_start: u64, x_end: u64, y_min: f64, y_max: f64,
) -> Result<PlotPath, String> {
    if x_start >= x_end { return Err("Plot window end must follow its start.".into()); }
    if !y_min.is_finite() || !y_max.is_finite() || y_min > y_max {
        return Err("Plot Y bounds must be finite and ordered.".into());
    }
    let mut valid: Vec<(u64, f64)> = points.iter().copied()
        .filter(|(time, value)| x_start <= *time && *time <= x_end && value.is_finite())
        .collect();
    if valid.is_empty() { return Err("No finite numeric samples within the plot window.".into()); }
    valid.sort_by_key(|point| point.0);
    render_path(valid, max_points, x_start, x_end, y_min, y_max)
}

fn render_path(
    valid: Vec<(u64, f64)>, max_points: usize,
    axis_start: u64, axis_end: u64, axis_min: f64, axis_max: f64,
) -> Result<PlotPath, String> {
    let first_x = valid.first().expect("nonempty").0;
    let last_x = valid.last().expect("nonempty").0;
    let min_y = valid.iter().map(|point| point.1).fold(f64::INFINITY, f64::min);
    let max_y = valid.iter().map(|point| point.1).fold(f64::NEG_INFINITY, f64::max);
    let budget = max_points.clamp(4, 4_096);
    let selected = if valid.len() <= budget {
        valid.clone()
    } else {
        let mut selected = Vec::with_capacity(budget);
        selected.push(valid[0]);
        let interior = &valid[1..valid.len() - 1];
        let buckets = (budget - 2) / 2;
        for bucket in 0..buckets {
            let from = bucket * interior.len() / buckets;
            let through = (bucket + 1) * interior.len() / buckets;
            let slice = &interior[from..through];
            if slice.is_empty() { continue; }
            let min_at = slice.iter().enumerate().min_by(|a, b|
                a.1.1.total_cmp(&b.1.1)).expect("nonempty").0;
            let max_at = slice.iter().enumerate().max_by(|a, b|
                a.1.1.total_cmp(&b.1.1)).expect("nonempty").0;
            if min_at <= max_at {
                selected.push(slice[min_at]);
                if max_at != min_at { selected.push(slice[max_at]); }
            } else {
                selected.push(slice[max_at]);
                selected.push(slice[min_at]);
            }
        }
        selected.push(*valid.last().expect("nonempty"));
        selected
    };
    let span_x = axis_end.saturating_sub(axis_start);
    let span_y = axis_max - axis_min;
    let mut commands = String::with_capacity(selected.len() * 20);
    for (index, &(time, value)) in selected.iter().enumerate() {
        let x = if span_x > 0 {
            (time.saturating_sub(axis_start) as f64 / span_x as f64) * 1_000.0
        } else if selected.len() > 1 {
            index as f64 / (selected.len() - 1) as f64 * 1_000.0
        } else { 500.0 };
        let y = if span_y.is_finite() && span_y > 0.0 {
            ((axis_max - value) / span_y * 300.0).clamp(0.0, 300.0)
        } else { 150.0 };
        let command = if index == 0 { 'M' } else { 'L' };
        write!(&mut commands, "{command} {x:.2} {y:.2} ")
            .expect("writing to String cannot fail");
    }
    Ok(PlotPath {
        commands: commands.trim_end().into(),
        source_points: valid.len(), plotted_points: selected.len(),
        first_x, last_x, min_y, max_y,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CanId, DbcDatabase, FdFlags, FrameKind};

    fn fixture() -> DbcStack {
        let mut stack = DbcStack::new();
        stack.add_global(DbcDatabase::parse(
            "BO_ 291 Engine: 2 ECU\n SG_ RPM : 0|8@1+ (2,0) [0|510] \"rpm\" ECU\n"
        ).unwrap());
        stack
    }

    fn frame(timestamp: Option<u64>, id: u16, value: u8) -> CanFrame {
        CanFrame::new(timestamp, "can0", CanId::Standard(id), FrameKind::Data,
            vec![value, 0], false, FdFlags::default()).unwrap()
    }

    #[test]
    fn decoded_dbc_series_respects_identifier_and_real_timestamps() {
        let frames = vec![frame(Some(100), 0x123, 2), frame(Some(101), 0x456, 100),
            frame(Some(200), 0x123, 3)];
        let filter = TraceFilter::parse("", "S:123").unwrap();
        let series = extract_numeric_series(&frames, &fixture(), &filter, "RPM").unwrap();
        assert_eq!(series.axis, HorizontalAxis::TimestampMicroseconds);
        assert_eq!(series.unit, "rpm");
        assert_eq!(series.points, vec![(100, 4.0), (200, 6.0)]);
        assert!(extract_numeric_series(&frames, &fixture(), &filter, "UNKNOWN").is_err());
    }

    #[test]
    fn unknown_timestamps_switch_entire_plot_to_frame_index() {
        let frames = vec![frame(Some(100), 0x123, 2), frame(None, 0x123, 4)];
        let filter = TraceFilter::default();
        let series = extract_numeric_series(&frames, &fixture(), &filter, "RPM").unwrap();
        assert_eq!(series.axis, HorizontalAxis::FrameIndex);
        assert_eq!(series.points, vec![(0, 4.0), (1, 8.0)]);
    }

    #[test]
    fn downsample_preserves_spikes_bounds_and_both_endpoints() {
        let mut points = (0..1_000).map(|x| (x, 0.0)).collect::<Vec<_>>();
        points[503].1 = 100.0;
        points[505].1 = -100.0;
        let chart = plot_path(&points, 40).unwrap();
        assert_eq!(chart.source_points, 1_000);
        assert!(chart.plotted_points <= 40);
        assert_eq!((chart.min_y, chart.max_y), (-100.0, 100.0));
        assert!(chart.commands.contains("0.00"));
        assert!(chart.commands.contains("300.00"));
        assert_eq!((chart.first_x, chart.last_x), (0, 999));
    }

    #[test]
    fn constant_value_single_sample_and_nonfinite_handling() {
        let constant = plot_path(&[(10, 5.0), (20, 5.0)], 2).unwrap();
        assert!(constant.commands.contains("150.00"));
        let single = plot_path(&[(10, 1.0)], 20).unwrap();
        assert_eq!(single.commands, "M 500.00 150.00");
        assert!(plot_path(&[(1, f64::NAN), (2, f64::INFINITY)], 20).is_err());
    }
    #[test]
    fn shared_axes_align_different_series_in_time() {
        let a = plot_path_on_axes(&[(100, 0.0), (200, 10.0)], 100,
            100, 200, 0.0, 10.0).unwrap();
        let b = plot_path_on_axes(&[(150, 100.0), (200, 200.0)], 100,
            100, 200, 100.0, 200.0).unwrap();
        assert_eq!(a.commands, "M 0.00 300.00 L 1000.00 0.00");
        assert_eq!(b.commands, "M 500.00 300.00 L 1000.00 0.00");
        assert_eq!(b.first_x, 150);
    }

    #[test]
    fn explicit_plot_window_clips_and_preserves_extremes() {
        let mut samples = (0..2_000).map(|x| (x, 0.0)).collect::<Vec<_>>();
        samples[750].1 = 500.0;
        samples[755].1 = -500.0;
        let graph = plot_path_on_axes(&samples, 40, 500, 1_000, -500.0, 500.0).unwrap();
        assert_eq!(graph.source_points, 501);
        assert!(graph.plotted_points <= 40);
        assert!(graph.commands.contains("0.00"));
        assert!(graph.commands.contains("300.00"));
        assert!(plot_path_on_axes(&samples, 100, 1_001, 1_001, 0.0, 1.0).is_err());
        assert!(plot_path_on_axes(&samples, 100, 3_000, 4_000, 0.0, 1.0).is_err());
        assert!(plot_path_on_axes(&samples, 100, 0, 1_000, f64::NAN, 1.0).is_err());
    }

}
