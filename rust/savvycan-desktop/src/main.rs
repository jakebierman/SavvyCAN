use std::cell::RefCell;
use std::collections::VecDeque;
use std::error::Error;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use savvycan_core::{
    parse_format_rules, read_trace, summarize, write_candump, write_crtd, write_decoded_csv, write_gvret_csv, write_id_stats_csv, write_byte_activity_csv, write_socketcan_pcap, write_socketcan_pcapng,
    analyze_ids, analyze_byte_activity_matching, extract_numeric_series_batch, plot_path_on_axes_sorted, HorizontalAxis, NumericSeries, OfflineReplay, ReplayClock, describe_uds_like_payload, CanFrame, DbcDatabase, DbcStack, DecodedSignal,
    CanReceive, Direction, FrameKind, IdFilter, IsoTpAssembler, J1939BamAssembler, PayloadFormatter,
    PayloadFormatRouter, ReceiveError, TraceFilter,
};
use slint::{Model, ModelRc, VecModel, Timer, TimerMode};

#[cfg(target_os = "linux")]
use savvycan_transports::SocketCanReceive;
use savvycan_transports::{GvretSerialReceive, SlcanSerialReceive};

slint::include_modules!();

#[derive(Default)]
struct DecodeContext {
    dbc: Option<DbcStack>,
    formatter: Option<PayloadFormatter>,
    rules: Option<PayloadFormatRouter>,
}

#[derive(Default)]
struct ViewerState {
    frames: Vec<CanFrame>,
    context: DecodeContext,
    live: bool,
    history_dropped: usize,
    page_offset: usize,
    replay: Option<OfflineReplay>,
    replay_playing: bool,
    replay_speed_factor: f64,
    replay_last_tick: Option<Instant>,
}

fn decode_context(dbc_path: &str, format_spec: &str, rules_path: &str)
    -> Result<DecodeContext, Box<dyn Error>>
{
    let dbc = if dbc_path.trim().is_empty() {
        None
    } else {
        let mut stack = DbcStack::new();
        for entry in dbc_path.split(';').map(str::trim).filter(|entry| !entry.is_empty()) {
            let (scope, path) = entry.split_once('=').unwrap_or(("*", entry));
            let scope = scope.trim();
            let path = path.trim();
            if path.is_empty() { return Err("DBC association requires a file path".into()); }
            let dbc = DbcDatabase::parse(&std::fs::read_to_string(path)?)?;
            if scope == "*" {
                stack.add_global(dbc);
            } else {
                if scope.is_empty() { return Err("DBC interface cannot be empty".into()); }
                stack.add_for_interface(scope, dbc);
            }
        }
        Some(stack)
    };
    let formatter = if format_spec.trim().is_empty() { None }
        else { Some(PayloadFormatter::compile(format_spec, false)?) };
    let rules = if rules_path.trim().is_empty() { None }
        else { Some(parse_format_rules(&std::fs::read_to_string(rules_path)?)?) };
    Ok(DecodeContext { dbc, formatter, rules })
}

fn signal_text(signal: DecodedSignal) -> String {
    let value = signal.text.unwrap_or_else(|| signal.value.to_string());
    if signal.unit.is_empty() { format!("{}={value}", signal.name) }
    else { format!("{}={value} {}", signal.name, signal.unit) }
}

fn row(frame: &CanFrame, context: &DecodeContext) -> FrameRow {
    let timestamp = frame.timestamp_us.map(|us| {
        format!("{}.{:06}", us / 1_000_000, us % 1_000_000)
    }).unwrap_or_else(|| "—".into());
    let format = match frame.kind {
        FrameKind::RemoteRequest { .. } => "RTR".to_owned(),
        FrameKind::Data if frame.is_fd => {
            match (frame.fd_flags.bitrate_switch, frame.fd_flags.error_state_indicator) {
                (true, true) => "FD/BRS/ESI",
                (true, false) => "FD/BRS",
                (false, true) => "FD/ESI",
                (false, false) => "FD",
            }.to_owned()
        }
        FrameKind::Data => "CAN".to_owned(),
    };
    let payload = frame.data.iter()
        .map(|byte| format!("{byte:02X}")).collect::<Vec<_>>().join(" ");
    let (message_name, decoded_values) = match context.dbc.as_ref().map(|db| db.decode(frame)) {
        Some(Ok(Some(decoded))) => (
            decoded.name,
            decoded.signals.into_iter().map(signal_text).collect::<Vec<_>>().join("  |  "),
        ),
        Some(Err(error)) => (String::new(), format!("Decode error: {error}")),
        _ => (String::new(), String::new()),
    };
    let typed_values = if matches!(frame.kind, FrameKind::Data) {
        context.rules.as_ref().and_then(|router|
            router.formatter_for(&frame.interface, frame.id))
            .or(context.formatter.as_ref())
            .map(|formatter| formatter.format(&frame.data)).unwrap_or_default()
    } else { String::new() };
    FrameRow {
        timestamp: timestamp.into(),
        channel: frame.interface.clone().into(),
        can_id: frame.id.to_string().into(),
        direction: match frame.direction {
            Direction::Received => "Rx",
            Direction::Transmitted => "Tx",
            Direction::Unknown => "—",
        }.into(),
        format: format.into(),
        length: frame.len().to_string().into(),
        payload: payload.into(),
        message_name: message_name.into(),
        decoded_values: decoded_values.into(),
        typed_values: typed_values.into(),
    }
}

fn visible_rows(state: &ViewerState, filter: &TraceFilter, limit: usize) -> Vec<FrameRow> {
    if let Some(replay) = state.replay.as_ref() {
        replay.recent_indices(limit).iter()
            .filter_map(|index| state.frames.get(*index))
            .map(|frame| row(frame, &state.context)).collect()
    } else if state.live {
        let mut rows = state.frames.iter().rev().filter(|frame| filter.matches(frame))
            .take(limit).map(|frame| row(frame, &state.context)).collect::<Vec<_>>();
        rows.reverse();
        rows
    } else {
        state.frames.iter().filter(|frame| filter.matches(frame))
            .skip(state.page_offset).take(limit)
            .map(|frame| row(frame, &state.context)).collect()
    }
}

fn filtered_count(state: &ViewerState, filter: &TraceFilter) -> usize {
    state.frames.iter().filter(|frame| filter.matches(frame)).count()
}

fn offline_page_text(state: &ViewerState, filter: &TraceFilter, page_size: usize) -> String {
    if let Some(replay) = state.replay.as_ref() {
        let clock = match replay.clock() {
            ReplayClock::SourceTimestamps => "capture timestamps",
            ReplayClock::FixedTenMilliseconds => "10 ms/frame fallback",
        };
        return format!("Offline replay • {} / {} frames • {:.3}/{:.3} s • {clock}",
            replay.position(), replay.total(), replay.elapsed_us() as f64 / 1e6,
            replay.duration_us() as f64 / 1e6);
    }
    let matching = filtered_count(state, filter);
    let first = if matching == 0 { 0 } else { state.page_offset + 1 };
    let last = matching.min(state.page_offset.saturating_add(page_size));
    format!("Rows {first}–{last} of {matching} matching • {} total frames", state.frames.len())
}



fn traffic_statistics_rows(frames: &[CanFrame], limit: usize) -> (Vec<StatsRow>, usize) {
    let mut groups = analyze_ids(frames);
    let all = groups.len();
    groups.sort_by(|a, b| b.count.cmp(&a.count)
        .then_with(|| a.interface.cmp(&b.interface))
        .then_with(|| a.id.value().cmp(&b.id.value()))
        .then_with(|| a.id.is_extended().cmp(&b.id.is_extended())));
    let rows = groups.into_iter().take(limit).map(|stat| {
        let format_time = |v: Option<u64>| v
            .map(|us| format!("{}.{:06}", us / 1_000_000, us % 1_000_000))
            .unwrap_or_else(|| "—".into());
        StatsRow {
            channel: stat.interface.into(),
            can_id: stat.id.to_string().into(),
            count: stat.count.to_string().into(),
            classic: stat.classic_count.to_string().into(),
            fd: stat.fd_count.to_string().into(),
            remote: stat.remote_count.to_string().into(),
            payload_bytes: stat.payload_bytes.to_string().into(),
            first: format_time(stat.first_us).into(),
            last: format_time(stat.last_us).into(),
            mean_period: stat.average_period_us
                .map(|v| format!("{v:.1}")).unwrap_or_else(|| "—".into()).into(),
            hz: stat.estimated_hz
                .map(|v| format!("{v:.2}")).unwrap_or_else(|| "—".into()).into(),
        }
    }).collect();
    (rows, all)
}


fn passive_bam_rows(
    state: &ViewerState, interface: &str, source: Option<u8>, limit: usize,
) -> (Vec<BamRow>, String) {
    let mut assembler = J1939BamAssembler::new();
    let mut rows = VecDeque::new();
    let mut reconstructed = 0usize;
    let mut invalid = 0usize;
    let mut examples = Vec::new();
    for (index, frame) in state.frames.iter().enumerate() {
        if !interface.is_empty() && frame.interface != interface { continue; }
        if let Some(source) = source {
            if !matches!(frame.id, savvycan_core::CanId::Extended(raw) if (raw & 0xff) as u8 == source) {
                continue;
            }
        }
        match assembler.push(frame) {
            Ok(Some(packet)) => {
                reconstructed += 1;
                let preview = packet.payload.iter().take(64)
                    .map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(" ");
                let payload = if packet.payload.len() > 64 {
                    format!("{preview} … ({} more bytes)", packet.payload.len() - 64)
                } else { preview };
                if rows.len() == limit { rows.pop_front(); }
                let timestamp = packet.end_timestamp_us
                    .map(|us| format!("{}.{:06}", us / 1_000_000, us % 1_000_000))
                    .unwrap_or_else(|| "—".into());
                rows.push_back(BamRow {
                    timestamp: timestamp.into(),
                    channel: packet.interface.into(),
                    source: format!("{:02X}", packet.source).into(),
                    pgn: format!("{:05X}", packet.pgn).into(),
                    length: packet.payload.len().to_string().into(),
                    payload: payload.into(),
                });
            }
            Ok(None) => {}
            Err(error) => {
                invalid += 1;
                if examples.len() < 3 { examples.push(format!("frame {}: {error}", index + 1)); }
            }
        }
    }
    let trailing = if examples.is_empty() { String::new() }
        else { format!(" • examples: {}", examples.join("; ")) };
    let summary = format!(
        "Passive J1939 BAM • {reconstructed} messages • {invalid} invalid/orphan packets • \
         {} incomplete sessions • latest {} shown{trailing}",
        assembler.pending_count(), rows.len()
    );
    (rows.into_iter().collect(), summary)
}

fn passive_isotp_rows(
    state: &ViewerState, filter: &TraceFilter, limit: usize,
) -> (Vec<IsoTpRow>, String) {
    let mut assembler = IsoTpAssembler::new();
    let mut visible = VecDeque::new();
    let mut checked = 0usize;
    let mut reconstructed = 0usize;
    let mut invalid = 0usize;
    let mut first_errors = Vec::new();
    for (index, frame) in state.frames.iter().enumerate() {
        if !filter.matches(frame) { continue; }
        checked += 1;
        match assembler.push(frame) {
            Ok(Some(packet)) => {
                reconstructed += 1;
                let bytes = packet.payload.iter().take(64)
                    .map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(" ");
                let payload = if packet.payload.len() > 64 {
                    format!("{bytes} … ({} more bytes)", packet.payload.len() - 64)
                } else { bytes };
                let timestamp = packet.end_timestamp_us
                    .map(|us| format!("{}.{:06}", us / 1_000_000, us % 1_000_000))
                    .unwrap_or_else(|| "—".into());
                if visible.len() == limit { visible.pop_front(); }
                visible.push_back(IsoTpRow {
                    timestamp: timestamp.into(),
                    channel: packet.interface.into(),
                    can_id: packet.id.to_string().into(),
                    direction: format!("{:?}", packet.direction).into(),
                    length: packet.payload.len().to_string().into(),
                    payload: payload.into(),
                    summary: describe_uds_like_payload(&packet.payload).into(),
                });
            }
            Ok(None) => {}
            Err(error) => {
                invalid += 1;
                if first_errors.len() < 3 {
                    first_errors.push(format!("frame {}: {error}", index + 1));
                }
            }
        }
    }
    let unfinished = assembler.pending_count();
    let notice = if invalid > 0 {
        format!(" • examples: {}", first_errors.join("; "))
    } else { String::new() };
    let message = format!(
        "Passive ISO-TP • {checked} frames inspected • {reconstructed} packets • \
         {invalid} errors • {unfinished} unfinished • latest {} shown{notice}",
        visible.len()
    );
    (visible.into_iter().collect(), message)
}

fn open_trace(path: &str, dbc_path: &str, format_spec: &str, rules_path: &str)
    -> Result<(ViewerState, String), Box<dyn Error>>
{
    let context = decode_context(dbc_path, format_spec, rules_path)?;
    let imported = read_trace(BufReader::new(File::open(path)?))?;
    let frames = imported.frames;
    let summary = summarize(&frames);
    let message = format!(
        "{} frames • {} interface/ID pairs • {} classic • {} FD • {} RTR • {}{}{}{}",
        summary.frames, summary.unique_identifiers, summary.classic_frames,
        summary.fd_frames, summary.remote_requests, imported.format,
        if let Some(stack) = &context.dbc {
            if stack.total_databases() == 1 { " • 1 DBC" } else { " • multiple DBCs" }
        } else { "" },
        if context.formatter.is_some() { " • default formatter" } else { "" },
        if context.rules.is_some() { " • per-ID rules" } else { "" },
    );
    Ok((ViewerState { frames, context, live: false, history_dropped: 0, page_offset: 0, ..Default::default() }, message))
}

/// Apply the current query across all retained frames, regardless of UI page.
fn selected_export_frames<'a>(
    frames: &'a [CanFrame], filter: Option<&TraceFilter>,
) -> std::borrow::Cow<'a, [CanFrame]> {
    match filter {
        Some(filter) if !filter.is_empty() => std::borrow::Cow::Owned(
            frames.iter().filter(|frame| filter.matches(frame)).cloned().collect()
        ),
        _ => std::borrow::Cow::Borrowed(frames),
    }
}

fn export_trace(
    path: &str, state: &ViewerState, filter: Option<&TraceFilter>,
) -> Result<(Option<usize>, usize), Box<dyn Error>> {
    if state.frames.is_empty() { return Err("Load a non-empty trace before exporting".into()); }
    if path.trim().is_empty() { return Err("Enter an export path".into()); }
    // Never page-limit exports. Unfiltered exports borrow without a copy.
    let frames = selected_export_frames(&state.frames, filter);
    if frames.is_empty() { return Err("No frames match the current export filter".into()); }
    let mut bytes = Vec::new();
    let lower = path.to_ascii_lowercase();
    let signal_count = if lower.ends_with(".bytes.csv") {
        Some(write_byte_activity_csv(&mut bytes, frames.as_ref())?)
    } else if lower.ends_with(".stats.csv") {
        Some(write_id_stats_csv(&mut bytes, frames.as_ref())?)
    } else if lower.ends_with(".signals.csv") {
        let dbc = state.context.dbc.as_ref()
            .ok_or("Load at least one DBC before exporting decoded signals")?;
        Some(write_decoded_csv(&mut bytes, frames.as_ref(), dbc)?)
    } else if lower.ends_with(".crtd") || lower.ends_with(".crt") {
        write_crtd(&mut bytes, frames.as_ref())?;
        None
    } else if lower.ends_with(".csv") {
        write_gvret_csv(&mut bytes, frames.as_ref())?;
        None
    } else if lower.ends_with(".log") || lower.ends_with(".candump") {
        write_candump(&mut bytes, frames.as_ref())?;
        None
    } else if lower.ends_with(".pcapng") {
        write_socketcan_pcapng(&mut bytes, frames.as_ref())?;
        None
    } else if lower.ends_with(".pcap") {
        write_socketcan_pcap(&mut bytes, frames.as_ref())?;
        None
    } else {
        return Err("Use .crt/.crtd, .csv, .log, .candump, .pcap, .pcapng, .stats.csv, .bytes.csv or .signals.csv for export".into());
    };
    // No overwrite and no partial output on format-validation failure.
    let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(&bytes)?;
    Ok((signal_count, frames.len()))
}

fn stop_capture(flag: &Rc<RefCell<Option<Arc<AtomicBool>>>>) {
    if let Some(running) = flag.borrow_mut().take() {
        running.store(false, Ordering::Relaxed);
    }
}

/// Interpret optional graph bounds relative to the earliest selected DBC
/// observation. Timestamped plots use seconds; timestamp-less plots use
/// integer frame offsets. This deliberately avoids combining the two units.
fn plot_window_bounds(
    first: u64, last: u64, axis: HorizontalAxis,
    from: &str, through: &str,
) -> Result<(u64, u64), String> {
    let offset = |raw: &str| -> Result<Option<u64>, String> {
        if raw.trim().is_empty() { return Ok(None); }
        let parsed = raw.trim().parse::<f64>()
            .map_err(|_| "Plot window must contain numeric bounds.".to_owned())?;
        if !parsed.is_finite() || parsed < 0.0 {
            return Err("Plot window bounds must be non-negative and finite.".into());
        }
        let scale = if axis == HorizontalAxis::TimestampMicroseconds {
            1_000_000.0
        } else { 1.0 };
        let scaled = parsed * scale;
        if !scaled.is_finite() || scaled >= u64::MAX as f64 {
            return Err("Plot window bound is too large.".into());
        }
        if axis == HorizontalAxis::FrameIndex && parsed.fract() != 0.0 {
            return Err("Frame-index plot bounds must be whole numbers.".into());
        }
        Ok(Some(scaled.round() as u64))
    };
    let start = match offset(from)? {
        Some(offset) => first.checked_add(offset)
            .ok_or("Plot start overflows trace time.".to_owned())?,
        None => first,
    };
    let end = match offset(through)? {
        Some(offset) => first.checked_add(offset)
            .ok_or("Plot end overflows trace time.".to_owned())?,
        None => last.max(first.saturating_add(1)),
    };
    if start >= end {
        return Err("Plot window end must be greater than start.".into());
    }
    Ok((start, end))
}

#[derive(Clone, Copy, Debug)]
struct PlotNavigation {
    first: u64,
    last: u64,
    start: u64,
    end: u64,
    axis: HorizontalAxis,
}

/// Navigation always stays within the original decoded capture, even when
/// the user originally entered a cropped plotting window.
fn navigate_plot_window(window: PlotNavigation, action: &str) -> Result<(u64, u64), String> {
    let last = window.last.max(window.first.saturating_add(1));
    let full = last - window.first;
    let span = window.end.saturating_sub(window.start).max(1).min(full);
    let clamp_start = |start: u64, width: u64| start.clamp(window.first, last - width);
    let bounds = match action {
        "fit" => (window.first, last),
        "left" => {
            let offset = (span / 4).max(1);
            let start = clamp_start(window.start.saturating_sub(offset), span);
            (start, start + span)
        }
        "right" => {
            let offset = (span / 4).max(1);
            let start = clamp_start(window.start.saturating_add(offset), span);
            (start, start + span)
        }
        "in" | "out" => {
            let width = if action == "in" { (span / 2).max(1) }
                else { span.saturating_mul(2).min(full) };
            let centre = window.start.saturating_add(
                window.end.saturating_sub(window.start) / 2);
            let start = clamp_start(centre.saturating_sub(width / 2), width);
            (start, start + width)
        }
        _ => return Err("Unknown plot navigation action.".into()),
    };
    Ok(bounds)
}

/// Wheel zoom anchors the point under the pointer, not the viewport centre.
/// Calculations are relative to the first sample to retain precision even
/// for large absolute microsecond timestamps.
fn zoom_plot_at(window: PlotNavigation, wheel_delta: f64, fraction: f64) -> (u64, u64) {
    if !wheel_delta.is_finite() || !fraction.is_finite() || wheel_delta == 0.0 {
        return (window.start, window.end);
    }
    let last = window.last.max(window.first.saturating_add(1));
    let full = last - window.first;
    let span = window.end.saturating_sub(window.start).max(1).min(full);
    let fraction = fraction.clamp(0.0, 1.0);
    // Mouse wheels report roughly 120 units per notch; trackpads send smaller deltas.
    let steps = (wheel_delta.abs() / 120.0).clamp(0.2, 6.0);
    let factor = 1.4_f64.powf(if wheel_delta > 0.0 { steps } else { -steps });
    let width = ((span as f64 * factor).round() as u64).clamp(1, full);
    let anchor = (window.start - window.first) as f64 + fraction * span as f64;
    let offset = (anchor - fraction * width as f64)
        .round().clamp(0.0, (full - width) as f64) as u64;
    let start = window.first + offset;
    (start, start + width)
}

/// Dragging right moves the visible window left (content follows the pointer).
fn pan_plot_by_fraction(window: PlotNavigation, drag_fraction: f64) -> (u64, u64) {
    if !drag_fraction.is_finite() || drag_fraction == 0.0 {
        return (window.start, window.end);
    }
    let last = window.last.max(window.first.saturating_add(1));
    let full = last - window.first;
    let span = window.end.saturating_sub(window.start).max(1).min(full);
    let offset = ((window.start - window.first) as f64 - drag_fraction * span as f64)
        .round().clamp(0.0, (full - span) as f64) as u64;
    let start = window.first + offset;
    (start, start + span)
}

fn plot_cursor_readout(session: &PlotSession, window: PlotNavigation, fraction: f64) -> String {
    if !fraction.is_finite() || fraction < 0.0 {
        return "Hover the plot to inspect decoded signal samples.".into();
    }
    let fraction = fraction.clamp(0.0, 1.0);
    let x = window.start.saturating_add(
        ((window.end - window.start) as f64 * fraction).round() as u64);
    let unit = if session.axis == HorizontalAxis::TimestampMicroseconds { "s" }
        else { "frames" };
    let mut fields = vec![format!(
        "Cursor +{} {unit}", plot_relative_offset(x, session.first, session.axis))];
    for signal in &session.series {
        let from = signal.points.partition_point(|(time, _)| *time < window.start);
        let to = signal.points.partition_point(|(time, _)| *time <= window.end);
        let visible = &signal.points[from..to];
        if visible.is_empty() {
            fields.push(format!("{}: —", signal.signal));
            continue;
        }
        let right = visible.partition_point(|(time, _)| *time < x);
        let best = if right == 0 { &visible[0] }
            else if right == visible.len() { &visible[right - 1] }
            else if visible[right - 1].0.abs_diff(x) <= visible[right].0.abs_diff(x) {
                &visible[right - 1]
            } else { &visible[right] };
        let suffix = if signal.unit.is_empty() { String::new() }
            else { format!(" {}", signal.unit) };
        fields.push(format!("{}: {:.3}{} (+{} {unit})", signal.signal, best.1,
            suffix, plot_relative_offset(best.0, session.first, session.axis)));
    }
    fields.join("  |  ")
}

/// Apply a new viewport without decoding CAN frames again.
fn redraw_plot_viewport(
    ui: &AppWindow, session: &PlotSession, old: PlotNavigation,
    start: u64, end: u64,
) -> Result<String, String> {
    let label = draw_plot_session(ui, session, start, end)?;
    ui.set_plot_window_from(plot_relative_offset(start, old.first, old.axis).into());
    ui.set_plot_window_through(plot_relative_offset(end, old.first, old.axis).into());
    ui.set_plot_cursor_fraction(-1.0);
    ui.set_plot_cursor_summary("Hover the plot to inspect decoded signal samples.".into());
    Ok(label)
}

fn plot_relative_offset(value: u64, first: u64, axis: HorizontalAxis) -> String {
    let relative = value.saturating_sub(first);
    match axis {
        HorizontalAxis::TimestampMicroseconds =>
            format!("{}.{:06}", relative / 1_000_000, relative % 1_000_000),
        HorizontalAxis::FrameIndex => relative.to_string(),
    }
}

/// Decoded signal observations are kept across navigation commands.
/// The next explicit Plot action replaces the cache after decoding a new
/// selection. Replotting a viewport never queries the bus or re-decodes CAN.
struct PlotSession {
    series: Vec<NumericSeries>,
    first: u64,
    last: u64,
    axis: HorizontalAxis,
    independent: bool,
}

fn draw_plot_session(
    ui: &AppWindow, session: &PlotSession, x_start: u64, x_end: u64,
) -> Result<String, String> {
    let ranges = session.series.iter().map(|signal| {
        let from = signal.points.partition_point(|(x, _)| *x < x_start);
        let to = signal.points.partition_point(|(x, _)| *x <= x_end);
        signal.points[from..to].iter()
            .fold(None, |acc: Option<(f64, f64, usize)>, (_, value)| {
                Some(match acc {
                    Some((min, max, count)) => (min.min(*value), max.max(*value), count + 1),
                    None => (*value, *value, 1),
                })
            })
    }).collect::<Vec<_>>();
    let shared_min = ranges.iter().flatten().map(|(min, _, _)| *min)
        .fold(f64::INFINITY, f64::min);
    let shared_max = ranges.iter().flatten().map(|(_, max, _)| *max)
        .fold(f64::NEG_INFINITY, f64::max);
    let mut commands = [String::new(), String::new(), String::new()];
    let mut legends = [String::new(), String::new(), String::new()];
    let mut all_samples = 0usize;
    for (index, signal) in session.series.iter().enumerate() {
        let Some((minimum, maximum, count)) = ranges[index] else {
            legends[index] = format!("{}: no samples in window", signal.signal);
            continue;
        };
        let (lower, upper) = if session.independent {
            (minimum, maximum)
        } else { (shared_min, shared_max) };
        let plot = plot_path_on_axes_sorted(&signal.points, 800,
            x_start, x_end, lower, upper)?;
        commands[index] = plot.commands;
        all_samples += count;
        let units = if signal.unit.is_empty() { String::new() }
            else { format!(" {}", signal.unit) };
        legends[index] = format!("{}: {:.3}–{:.3}{} ({} samples)",
            signal.signal, minimum, maximum, units, count);
    }
    let divisor = if session.axis == HorizontalAxis::TimestampMicroseconds {
        1_000_000.0
    } else { 1.0 };
    let unit = if session.axis == HorizontalAxis::TimestampMicroseconds {
        "seconds"
    } else { "frame offsets" };
    let label = format!(
        "{} signals • {} visible samples • {:.3}–{:.3} {} since first sample • {} Y scales",
        session.series.len(), all_samples,
        x_start.saturating_sub(session.first) as f64 / divisor,
        x_end.saturating_sub(session.first) as f64 / divisor,
        unit, if session.independent { "independent" } else { "shared" }
    );
    ui.set_plot_commands(commands[0].clone().into());
    ui.set_plot_commands_b(commands[1].clone().into());
    ui.set_plot_commands_c(commands[2].clone().into());
    ui.set_plot_legend_a(legends[0].clone().into());
    ui.set_plot_legend_b(legends[1].clone().into());
    ui.set_plot_legend_c(legends[2].clone().into());
    ui.set_plot_summary(label.clone().into());
    ui.set_show_isotp(false);
    ui.set_show_stats(false);
    ui.set_show_bam(false);
    ui.set_show_bytes(false);
    ui.set_show_plot(true);
    Ok(label)
}

type ImportResult = Result<(ViewerState, String), String>;

fn main() -> Result<(), Box<dyn Error>> {
    const OFFLINE_ROWS: usize = 1_000;
    const LIVE_ROWS: usize = 2_000;
    const LIVE_HISTORY_LIMIT: usize = 100_000;

    let ui = AppWindow::new()?;
    let model = Rc::new(VecModel::<FrameRow>::from(Vec::new()));
    let isotp_model = Rc::new(VecModel::<IsoTpRow>::from(Vec::new()));
    ui.set_isotp_rows(ModelRc::from(Rc::clone(&isotp_model)));
    let stats_model = Rc::new(VecModel::<StatsRow>::from(Vec::new()));
    ui.set_stats_rows(ModelRc::from(Rc::clone(&stats_model)));
    let bam_model = Rc::new(VecModel::<BamRow>::from(Vec::new()));
    ui.set_bam_rows(ModelRc::from(Rc::clone(&bam_model)));
    let byte_model = Rc::new(VecModel::<ByteActivityRow>::from(Vec::new()));
    ui.set_byte_rows(ModelRc::from(Rc::clone(&byte_model)));
    ui.set_frames(ModelRc::from(Rc::clone(&model)));
    let state = Rc::new(RefCell::new(ViewerState::default()));
    let filter = Rc::new(RefCell::new(TraceFilter::default()));
    let capture_flag: Rc<RefCell<Option<Arc<AtomicBool>>>> = Rc::new(RefCell::new(None));
    let receiver: Rc<RefCell<Option<mpsc::Receiver<Result<CanFrame, ReceiveError>>>>> =
        Rc::new(RefCell::new(None));
    let import_rx: Rc<RefCell<Option<mpsc::Receiver<ImportResult>>>> =
        Rc::new(RefCell::new(None));

    // Native OS file pickers only select paths. All reads and writes retain
    // their existing validation; export still refuses to overwrite files.
    let weak = ui.as_weak();
    ui.on_browse_trace(move || {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("CAN traces", &["log", "trc", "asc", "crtd", "csv", "pcap", "pcapng"])
            .add_filter("All files", &["*"])
            .pick_file()
        {
            if let Some(ui) = weak.upgrade() {
                ui.set_file_path(path.to_string_lossy().into_owned().into());
            }
        }
    });

    let weak = ui.as_weak();
    ui.on_browse_dbc(move || {
        if let Some(paths) = rfd::FileDialog::new()
            .add_filter("CAN database", &["dbc"])
            .pick_files()
        {
            if let Some(ui) = weak.upgrade() {
                let paths = paths.iter().map(|path| path.to_string_lossy().into_owned())
                    .collect::<Vec<_>>().join(";");
                ui.set_dbc_path(paths.into());
            }
        }
    });

    let weak = ui.as_weak();
    ui.on_browse_rules(move || {
        if let Some(path) = rfd::FileDialog::new().pick_file() {
            if let Some(ui) = weak.upgrade() {
                ui.set_rules_path(path.to_string_lossy().into_owned().into());
            }
        }
    });

    let weak = ui.as_weak();
    ui.on_browse_export(move || {
        if let Some(path) = rfd::FileDialog::new()
            .set_file_name("capture.pcapng")
            .add_filter("PCAPNG capture", &["pcapng"])
            .add_filter("PCAP capture", &["pcap"])
            .add_filter("CAN logs", &["log", "crtd", "csv"])
            .save_file()
        {
            if let Some(ui) = weak.upgrade() {
                ui.set_export_path(path.to_string_lossy().into_owned().into());
                ui.set_status_text(
                    "Export path selected. Click Export trace to create a NEW file.".into()
                );
            }
        }
    });

    let weak = ui.as_weak();
    let import_for_open = Rc::clone(&import_rx);
    ui.on_open_trace(move |path, dbc_path, format_spec, rules_path| {
        if let Some(ui) = weak.upgrade() {
            if path.trim().is_empty() {
                ui.set_status_text("Enter a CAN trace path first.".into());
                return;
            }
            let (sender, rx) = mpsc::sync_channel(1);
            *import_for_open.borrow_mut() = Some(rx);
            let (path, dbc_path, format_spec, rules_path) = (
                path.to_string(), dbc_path.to_string(), format_spec.to_string(),
                rules_path.to_string()
            );
            std::thread::spawn(move || {
                let result = open_trace(&path, &dbc_path, &format_spec, &rules_path)
                    .map_err(|error| error.to_string());
                let _ = sender.send(result);
            });
            ui.set_status_text("Loading CAN trace and decoding context in background…".into());
        }
    });

    let weak = ui.as_weak();
    let state_for_export = Rc::clone(&state);
    let filter_for_export = Rc::clone(&filter);
    ui.on_export_trace(move |path, matching| {
        if let Some(ui) = weak.upgrade() {
            let state = state_for_export.borrow();
            let filter = filter_for_export.borrow();
            match export_trace(path.as_str(), &state, matching.then_some(&*filter)) {
                Ok((Some(rows), count)) => {
                    let kind = if path.to_ascii_lowercase().ends_with(".bytes.csv") {
                        "per-byte CAN activity rows"
                    } else if path.to_ascii_lowercase().ends_with(".stats.csv") {
                        "CAN interface/ID statistics groups"
                    } else { "decoded DBC signal values" };
                    ui.set_status_text(format!("Exported {rows} {kind} from {count} frames to {path}").into());
                },
                Ok((None, count)) => {
                    let candump = path.to_ascii_lowercase().ends_with(".log")
                        || path.to_ascii_lowercase().ends_with(".candump");
                    let caveat = if candump { " • candump does not store Rx/Tx direction" }
                        else if path.to_ascii_lowercase().ends_with(".pcap") {
                            " • PCAP omits interface and Rx/Tx direction"
                        } else { "" };
                    ui.set_status_text(format!("Exported {count} frames to {path}{caveat}").into());
                },
                Err(error) => ui.set_status_text(format!("Could not export: {error}").into()),
            }
        }
    });

    let weak = ui.as_weak();
    let state_for_filter = Rc::clone(&state);
    let model_for_filter = Rc::clone(&model);
    let filter_for_apply = Rc::clone(&filter);
    ui.on_apply_filter(move |interface, id, from, through, payload| {
        if let Some(ui) = weak.upgrade() {
            match TraceFilter::parse_with_time_and_payload(interface.as_str(), id.as_str(),
                from.as_str(), through.as_str(), payload.as_str()) {
                Ok(new_filter) => {
                    let mut state = state_for_filter.borrow_mut();
                    state.page_offset = 0;
                    state.replay = None;
                    state.replay_playing = false;
                    state.replay_last_tick = None;
                    let limit = if state.live { LIVE_ROWS } else { OFFLINE_ROWS };
                    let count = filtered_count(&state, &new_filter);
                    model_for_filter.set_vec(visible_rows(&state, &new_filter, limit));
                    ui.set_page_text(if state.live {
                        "Live: latest 2,000 matching rows".into()
                    } else { offline_page_text(&state, &new_filter, OFFLINE_ROWS).into() });
                    *filter_for_apply.borrow_mut() = new_filter;
                    ui.set_status_text(format!("{count} matching frames • {} shown • filter active",
                        model_for_filter.row_count()).into());
                }
                Err(error) => ui.set_status_text(format!("Invalid trace filter: {error}").into()),
            }
        }
    });

    let weak = ui.as_weak();
    let state_for_previous = Rc::clone(&state);
    let filter_for_previous = Rc::clone(&filter);
    let model_for_previous = Rc::clone(&model);
    ui.on_previous_page(move || {
        if let Some(ui) = weak.upgrade() {
            let mut state = state_for_previous.borrow_mut();
            if state.live {
                ui.set_status_text("Paging is available after stopping live capture.".into());
                return;
            }
            if state.replay.is_some() {
                ui.set_status_text("Stop offline replay before paging.".into());
                return;
            }
            state.page_offset = state.page_offset.saturating_sub(OFFLINE_ROWS);
            let filter = filter_for_previous.borrow();
            model_for_previous.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
            ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
        }
    });

    let weak = ui.as_weak();
    let state_for_next = Rc::clone(&state);
    let filter_for_next = Rc::clone(&filter);
    let model_for_next = Rc::clone(&model);
    ui.on_next_page(move || {
        if let Some(ui) = weak.upgrade() {
            let mut state = state_for_next.borrow_mut();
            if state.live {
                ui.set_status_text("Paging is available after stopping live capture.".into());
                return;
            }
            if state.replay.is_some() {
                ui.set_status_text("Stop offline replay before paging.".into());
                return;
            }
            let filter = filter_for_next.borrow();
            let next = state.page_offset.saturating_add(OFFLINE_ROWS);
            if next < filtered_count(&state, &filter) {
                state.page_offset = next;
            }
            model_for_next.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
            ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
        }
    });


    // Offline playback reveals stored frames only. No CAN interface or
    // sender is ever opened, so this cannot replay onto a vehicle bus.
    let weak = ui.as_weak();
    let state_for_replay = Rc::clone(&state);
    let filter_for_replay = Rc::clone(&filter);
    let model_for_replay = Rc::clone(&model);
    ui.on_play_replay(move |requested_speed| {
        let Some(ui) = weak.upgrade() else { return; };
        let speed = match requested_speed.trim().parse::<f64>() {
            Ok(value) if value.is_finite() && (0.1..=100.0).contains(&value) => value,
            _ => {
                ui.set_status_text("Replay speed must be between 0.1x and 100x.".into());
                return;
            }
        };
        let mut state = state_for_replay.borrow_mut();
        if state.live {
            ui.set_status_text("Stop live capture before replaying a trace.".into());
            return;
        }
        if state.frames.is_empty() {
            ui.set_status_text("Open a CAN trace before offline replay.".into());
            return;
        }
        let filter = filter_for_replay.borrow();
        if state.replay.is_none() {
            let replay = OfflineReplay::new(&state.frames, &filter);
            if replay.total() == 0 {
                ui.set_status_text("No frames match the current replay filter.".into());
                return;
            }
            state.replay = Some(replay);
        }
        let replay = state.replay.as_mut().expect("just populated replay");
        if replay.finished() { replay.reset(); }
        replay.advance(0);
        state.replay_speed_factor = speed;
        state.replay_playing = true;
        state.replay_last_tick = Some(Instant::now());
        model_for_replay.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
        ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
        ui.set_show_isotp(false);
        ui.set_show_stats(false);
        ui.set_show_bam(false);
        ui.set_show_bytes(false);
        ui.set_show_plot(false);
        ui.set_status_text(format!("Playing offline trace at {speed}x (never transmits CAN).").into());
    });

    let weak = ui.as_weak();
    let state_for_pause = Rc::clone(&state);
    ui.on_pause_replay(move || {
        let Some(ui) = weak.upgrade() else { return; };
        let mut state = state_for_pause.borrow_mut();
        if state.replay.is_some() {
            state.replay_playing = false;
            state.replay_last_tick = None;
            ui.set_status_text("Offline replay paused.".into());
        }
    });

    let weak = ui.as_weak();
    let state_for_step = Rc::clone(&state);
    let filter_for_step = Rc::clone(&filter);
    let model_for_step = Rc::clone(&model);
    ui.on_step_replay(move || {
        let Some(ui) = weak.upgrade() else { return; };
        let mut state = state_for_step.borrow_mut();
        if state.live || state.frames.is_empty() {
            ui.set_status_text("Stop live capture and open a trace before stepping.".into());
            return;
        }
        let filter = filter_for_step.borrow();
        if state.replay.is_none() {
            state.replay = Some(OfflineReplay::new(&state.frames, &filter));
        }
        state.replay_playing = false;
        state.replay_last_tick = None;
        if let Some(replay) = state.replay.as_mut() { replay.step_frames(1); }
        model_for_step.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
        ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
        ui.set_show_isotp(false);
        ui.set_show_stats(false);
        ui.set_show_bam(false);
        ui.set_show_bytes(false);
        ui.set_show_plot(false);
        ui.set_status_text("Offline replay stepped one matching frame.".into());
    });

    let weak = ui.as_weak();
    let state_for_stop_replay = Rc::clone(&state);
    let filter_for_stop_replay = Rc::clone(&filter);
    let model_for_stop_replay = Rc::clone(&model);
    ui.on_stop_replay(move || {
        let Some(ui) = weak.upgrade() else { return; };
        let mut state = state_for_stop_replay.borrow_mut();
        state.replay = None;
        state.replay_playing = false;
        state.replay_last_tick = None;
        state.page_offset = 0;
        let filter = filter_for_stop_replay.borrow();
        model_for_stop_replay.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
        ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
        ui.set_status_text("Offline replay stopped; full trace view restored.".into());
    });

    // Passive, explicitly selected diagnostic ID. Never transmits or
    // generates flow-control frames. Inspects full retained trace in
    // source order, independently of display filter and offline page.
    let weak = ui.as_weak();
    let state_for_isotp = Rc::clone(&state);
    let model_for_isotp = Rc::clone(&isotp_model);
    ui.on_inspect_isotp(move |interface, id| {
        if let Some(ui) = weak.upgrade() {
            if id.trim().is_empty() {
                ui.set_status_text("Select one exact ISO-TP CAN ID (e.g. S:7E8).".into());
                return;
            }
            match TraceFilter::parse(interface.as_str(), id.as_str()) {
                Ok(filter) if matches!(filter.id, Some(IdFilter::Exact(_))) => {
                    let state = state_for_isotp.borrow();
                    if state.live {
                        ui.set_status_text("Stop live capture before inspecting ISO-TP history.".into());
                        return;
                    }
                    let (rows, summary) = passive_isotp_rows(&state, &filter, 2_000);
                    model_for_isotp.set_vec(rows);
                    ui.set_isotp_summary(summary.clone().into());
                    ui.set_show_isotp(true);
                    ui.set_show_stats(false);
                        ui.set_show_bam(false);
                    ui.set_show_bytes(false);
                    ui.set_show_plot(false);
                    ui.set_status_text(summary.into());
                }
                Ok(_) => ui.set_status_text(
                    "ISO-TP requires an exact CAN ID, not a range, mask or PGN.".into()
                ),
                Err(error) => ui.set_status_text(format!("Invalid ISO-TP selector: {error}").into()),
            }
        }
    });


    let weak = ui.as_weak();
    let state_for_stats = Rc::clone(&state);
    let model_for_stats = Rc::clone(&stats_model);
    ui.on_open_statistics(move || {
        if let Some(ui) = weak.upgrade() {
            let state = state_for_stats.borrow();
            if state.live {
                ui.set_status_text("Stop live capture before generating traffic statistics.".into());
                return;
            }
            if state.frames.is_empty() {
                ui.set_status_text("Load a trace or stop a capture before viewing statistics.".into());
                return;
            }
            let (rows, total) = traffic_statistics_rows(&state.frames, 2_000);
            let shown = rows.len();
            model_for_stats.set_vec(rows);
            let message = format!(
                "{total} interface/ID groups • top {shown} shown by count • full report: .stats.csv"
            );
            ui.set_stats_summary(message.clone().into());
            ui.set_show_isotp(false);
            ui.set_show_stats(true);
            ui.set_show_bam(false);
                    ui.set_show_bytes(false);
                    ui.set_show_plot(false);
            ui.set_status_text(message.into());
        }
    });

    // On-demand payload-byte variability inspector. Uses the full retained
    // trace in capture order; never mutates samples or accesses a CAN device.
    let weak = ui.as_weak();
    let state_for_bytes = Rc::clone(&state);
    let model_for_bytes = Rc::clone(&byte_model);
    ui.on_inspect_bytes(move |interface, id| {
        let Some(ui) = weak.upgrade() else { return; };
        let selector = match TraceFilter::parse(interface.as_str(), id.as_str()) {
            Ok(query) => query,
            Err(error) => {
                ui.set_status_text(format!("Invalid byte activity selector: {error}").into());
                return;
            }
        };
        let state = state_for_bytes.borrow();
        if state.live {
            ui.set_status_text("Stop live capture before inspecting byte activity.".into());
            return;
        }
        if state.frames.is_empty() {
            ui.set_status_text("Load a trace before inspecting byte activity.".into());
            return;
        }
        let mut analyzed = analyze_byte_activity_matching(&state.frames, &selector);
        let all = analyzed.len();
        analyzed.sort_by(|a, b| b.bit_flips.cmp(&a.bit_flips)
            .then_with(|| b.changes.cmp(&a.changes))
            .then_with(|| b.samples.cmp(&a.samples))
            .then_with(|| a.interface.cmp(&b.interface))
            .then_with(|| a.id.value().cmp(&b.id.value()))
            .then_with(|| a.id.is_extended().cmp(&b.id.is_extended()))
            .then_with(|| a.byte_index.cmp(&b.byte_index)));
        let rows = analyzed.into_iter().take(256).map(|item| ByteActivityRow {
            channel: item.interface.into(),
            can_id: item.id.to_string().into(),
            position: item.byte_index.to_string().into(),
            samples: item.samples.to_string().into(),
            distinct: item.distinct_values.to_string().into(),
            changes: item.changes.to_string().into(),
            flips: item.bit_flips.to_string().into(),
            last: format!("{:02X}", item.last_value).into(),
        }).collect::<Vec<_>>();
        let shown = rows.len();
        model_for_bytes.set_vec(rows);
        let message = format!(
            "{all} byte positions • {shown} displayed by bit activity • export .bytes.csv for full report"
        );
        ui.set_byte_summary(message.clone().into());
        ui.set_show_isotp(false);
        ui.set_show_stats(false);
        ui.set_show_bam(false);
        ui.set_show_bytes(true);
        ui.set_status_text(message.into());
    });

    // Up to three passive decoded DBC trends, all on the same exact CAN ID
    // and shared X axis. Independent Y scales keep different engineering
    // units interpretable without suggesting they share a physical scale.
    let plot_navigation = Rc::new(RefCell::new(None::<PlotNavigation>));
    let plot_cache = Rc::new(RefCell::new(None::<PlotSession>));
    let cache_for_plot = Rc::clone(&plot_cache);
    let nav_for_plot = Rc::clone(&plot_navigation);
    let weak = ui.as_weak();
    let state_for_plot = Rc::clone(&state);
    ui.on_plot_signal_series(move |interface, id, names, from, through, independent| {
        let Some(ui) = weak.upgrade() else { return; };
        let selector = match TraceFilter::parse(interface.as_str(), id.as_str()) {
            Ok(filter) if matches!(filter.id, Some(IdFilter::Exact(_))) => filter,
            Ok(_) => {
                ui.set_status_text("Plot one exact 11/29-bit CAN ID, e.g. S:123.".into());
                return;
            }
            Err(error) => {
                ui.set_status_text(format!("Invalid plot CAN ID: {error}").into());
                return;
            }
        };
        let selected_names = names.as_str().split(|c: char| c == ',' || c == ';')
            .map(str::trim).filter(|name| !name.is_empty())
            .collect::<Vec<_>>();
        if selected_names.is_empty() || selected_names.len() > 3 {
            ui.set_status_text("Enter one to three DBC signal names, separated by commas.".into());
            return;
        }
        if selected_names.iter().enumerate().any(|(index, name)|
            selected_names[..index].contains(name))
        {
            ui.set_status_text("Enter each plotted signal name only once.".into());
            return;
        }
        let state = state_for_plot.borrow();
        if state.live {
            ui.set_status_text("Stop live capture before plotting DBC signals.".into());
            return;
        }
        let Some(dbc) = state.context.dbc.as_ref() else {
            ui.set_status_text("Load a DBC before plotting decoded signals.".into());
            return;
        };
        let mut series = match extract_numeric_series_batch(
            &state.frames, dbc, &selector, &selected_names) {
            Ok(series) => series,
            Err(error) => {
                ui.set_status_text(error.into());
                return;
            }
        };
        // Imported traces may contain out-of-order timestamps. Sort once per
        // selection so every subsequent pan/zoom/cursor lookup is viewport-local.
        for signal in &mut series { signal.points.sort_by_key(|point| point.0); }
        let axis = series[0].axis;
        if series.iter().any(|value| value.axis != axis) {
            ui.set_status_text("Selected signals have incompatible time bases; choose signals with consistent timestamps.".into());
            return;
        }
        if !independent && series.iter().any(|value| value.unit != series[0].unit) {
            ui.set_status_text("Shared Y scale requires matching signal units; enable independent Y scales.".into());
            return;
        }
        let first = series.iter().flat_map(|s| s.points.iter().map(|p| p.0))
            .min().expect("decoded series is nonempty");
        let last = series.iter().flat_map(|s| s.points.iter().map(|p| p.0))
            .max().expect("decoded series is nonempty");
        let (x_start, x_end) = match plot_window_bounds(first, last, axis,
            from.as_str(), through.as_str()) {
            Ok(window) => window,
            Err(error) => {
                ui.set_status_text(error.into());
                return;
            }
        };
        let session = PlotSession { series, first, last, axis, independent };
        let label = match draw_plot_session(&ui, &session, x_start, x_end) {
            Ok(label) => label,
            Err(error) => {
                ui.set_status_text(error.into());
                return;
            }
        };
        *cache_for_plot.borrow_mut() = Some(session);
        *nav_for_plot.borrow_mut() = Some(PlotNavigation {
            first, last, start: x_start, end: x_end, axis,
        });
        ui.set_plot_cursor_fraction(-1.0);
        ui.set_plot_cursor_summary("Hover the plot to inspect decoded signal samples.".into());
        ui.set_status_text(label.into());
    });

    let weak = ui.as_weak();
    let nav_for_controls = Rc::clone(&plot_navigation);
    let cache_for_controls = Rc::clone(&plot_cache);
    ui.on_navigate_plot(move |action| {
        let Some(ui) = weak.upgrade() else { return; };
        if !ui.get_show_plot() {
            ui.set_status_text("Plot DBC signals before navigating.".into());
            return;
        }
        let Some(window) = *nav_for_controls.borrow() else { return; };
        match navigate_plot_window(window, action.as_str()) {
            Ok((start, end)) => {
                let cache = cache_for_controls.borrow();
                let Some(session) = cache.as_ref() else { return; };
                match redraw_plot_viewport(&ui, session, window, start, end) {
                    Ok(label) => {
                        *nav_for_controls.borrow_mut() = Some(PlotNavigation {
                            start, end, ..window
                        });
                        ui.set_status_text(label.into());
                    }
                    Err(error) => ui.set_status_text(error.into()),
                }
            }
            Err(error) => ui.set_status_text(error.into()),
        }
    });

    let weak = ui.as_weak();
    let nav_for_wheel = Rc::clone(&plot_navigation);
    let cache_for_wheel = Rc::clone(&plot_cache);
    ui.on_zoom_plot(move |delta, fraction| {
        let Some(ui) = weak.upgrade() else { return; };
        if !ui.get_show_plot() { return; }
        let Some(window) = *nav_for_wheel.borrow() else { return; };
        let (start, end) = zoom_plot_at(window, delta as f64, fraction as f64);
        if (start, end) == (window.start, window.end) { return; }
        let cache = cache_for_wheel.borrow();
        let Some(session) = cache.as_ref() else { return; };
        match redraw_plot_viewport(&ui, session, window, start, end) {
            Ok(label) => {
                *nav_for_wheel.borrow_mut() = Some(PlotNavigation { start, end, ..window });
                ui.set_status_text(label.into());
            }
            Err(error) => ui.set_status_text(error.into()),
        }
    });

    let weak = ui.as_weak();
    let nav_for_drag = Rc::clone(&plot_navigation);
    let cache_for_drag = Rc::clone(&plot_cache);
    ui.on_drag_plot(move |fraction| {
        let Some(ui) = weak.upgrade() else { return; };
        if !ui.get_show_plot() { return; }
        let Some(window) = *nav_for_drag.borrow() else { return; };
        let (start, end) = pan_plot_by_fraction(window, fraction as f64);
        if (start, end) == (window.start, window.end) { return; }
        let cache = cache_for_drag.borrow();
        let Some(session) = cache.as_ref() else { return; };
        match redraw_plot_viewport(&ui, session, window, start, end) {
            Ok(label) => {
                *nav_for_drag.borrow_mut() = Some(PlotNavigation { start, end, ..window });
                ui.set_status_text(label.into());
            }
            Err(error) => ui.set_status_text(error.into()),
        }
    });

    let weak = ui.as_weak();
    let nav_for_cursor = Rc::clone(&plot_navigation);
    let cache_for_cursor = Rc::clone(&plot_cache);
    ui.on_inspect_plot(move |fraction| {
        let Some(ui) = weak.upgrade() else { return; };
        if !ui.get_show_plot() { return; }
        let Some(window) = *nav_for_cursor.borrow() else { return; };
        let cache = cache_for_cursor.borrow();
        let Some(session) = cache.as_ref() else { return; };
        let fraction = fraction as f64;
        ui.set_plot_cursor_fraction(
            if fraction.is_finite() && fraction >= 0.0 {
                fraction.clamp(0.0, 1.0) as f32
            } else { -1.0 });
        ui.set_plot_cursor_summary(plot_cursor_readout(session, window, fraction).into());
    });

    let weak = ui.as_weak();
    let state_for_start = Rc::clone(&state);
    let model_for_start = Rc::clone(&model);
    let flag_for_start = Rc::clone(&capture_flag);
    let receiver_for_start = Rc::clone(&receiver);
    let import_for_start = Rc::clone(&import_rx);
    ui.on_start_live(move |interface, dbc_path, format_spec, rules_path| {
        if let Some(ui) = weak.upgrade() {
            #[cfg(target_os = "linux")]
            {
                if interface.trim().is_empty() {
                    ui.set_status_text("Enter a SocketCAN interface (e.g. can0 or vcan0).".into());
                    return;
                }
                match decode_context(dbc_path.as_str(), format_spec.as_str(), rules_path.as_str()) {
                    Ok(context) => {
                        stop_capture(&flag_for_start);
                        *receiver_for_start.borrow_mut() = None;
                        *import_for_start.borrow_mut() = None;
                        let (sender, rx) = mpsc::sync_channel(4_096);
                        let running = Arc::new(AtomicBool::new(true));
                        let worker_running = Arc::clone(&running);
                        let interface = interface.to_string();
                        std::thread::spawn(move || {
                            match SocketCanReceive::open(&interface) {
                                Ok(mut source) => {
                                    while worker_running.load(Ordering::Relaxed) {
                                        match source.receive(Duration::from_millis(100)) {
                                            Ok(Some(frame)) => {
                                                if sender.send(Ok(frame)).is_err() { break; }
                                            }
                                            Ok(None) => {}
                                            Err(error) => {
                                                let _ = sender.send(Err(error));
                                                break;
                                            }
                                        }
                                    }
                                }
                                Err(error) => { let _ = sender.send(Err(error)); }
                            }
                        });
                        *flag_for_start.borrow_mut() = Some(running);
                        *receiver_for_start.borrow_mut() = Some(rx);
                        *state_for_start.borrow_mut() = ViewerState {
                            frames: Vec::new(), context, live: true, history_dropped: 0,
                            page_offset: 0, ..Default::default()
                        };
                        model_for_start.clear();
                        ui.set_show_isotp(false);
                        ui.set_show_stats(false);
                        ui.set_show_bam(false);
                    ui.set_show_bytes(false);
                    ui.set_show_plot(false);
                        ui.set_page_text("Live: latest 2,000 matching rows".into());
                        ui.set_status_text("Opening receive-only SocketCAN interface…".into());
                    }
                    Err(error) => ui.set_status_text(format!("Cannot start capture: {error}").into()),
                }
            }
            #[cfg(not(target_os = "linux"))]
            {
                let _ = (interface, dbc_path, format_spec, rules_path,
                    &state_for_start, &model_for_start, &flag_for_start, &receiver_for_start);
                ui.set_status_text("SocketCAN capture is currently Linux-only.".into());
            }
        }
    });



    let weak = ui.as_weak();
    let state_for_bam = Rc::clone(&state);
    let model_for_bam = Rc::clone(&bam_model);
    ui.on_inspect_bam(move |interface, source_text| {
        if let Some(ui) = weak.upgrade() {
            let source = if source_text.trim().is_empty() {
                None
            } else {
                let text = source_text.trim();
                let stripped = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X"))
                    .unwrap_or(text);
                match u8::from_str_radix(stripped, 16) {
                    Ok(value) => Some(value),
                    Err(_) => {
                        ui.set_status_text(
                            "BAM source address must be one hexadecimal byte (e.g. 80).".into()
                        );
                        return;
                    }
                }
            };
            let state = state_for_bam.borrow();
            if state.live {
                ui.set_status_text("Stop live capture before inspecting J1939 BAM.".into());
                return;
            }
            if state.frames.is_empty() {
                ui.set_status_text("Load a trace before inspecting J1939 BAM.".into());
                return;
            }
            let (rows, summary) = passive_bam_rows(&state, interface.trim(), source, 2_000);
            model_for_bam.set_vec(rows);
            ui.set_bam_summary(summary.clone().into());
            ui.set_show_isotp(false);
            ui.set_show_stats(false);
            ui.set_show_bam(true);
            ui.set_show_bytes(false);
                    ui.set_show_plot(false);
            ui.set_status_text(summary.into());
        }
    });

    // Explicit, preconfigured serial listener. The decoder does not send
    // CAN frames, device configuration or SLCAN open-channel commands.
    let weak = ui.as_weak();
    let state_for_slcan = Rc::clone(&state);
    let model_for_slcan = Rc::clone(&model);
    let flag_for_slcan = Rc::clone(&capture_flag);
    let receiver_for_slcan = Rc::clone(&receiver);
    let import_for_slcan = Rc::clone(&import_rx);
    ui.on_start_slcan(move |port, baud_text, interface, dbc_path, format_spec, rules_path| {
        if let Some(ui) = weak.upgrade() {
            let baud = match baud_text.trim().parse::<u32>() {
                Ok(value) if (9_600..=3_000_000).contains(&value) => value,
                _ => {
                    ui.set_status_text("SLCAN serial baud must be 9600–3000000.".into());
                    return;
                }
            };
            if port.trim().is_empty() || interface.trim().is_empty() {
                ui.set_status_text("Enter a serial path and interface label for SLCAN.".into());
                return;
            }
            match decode_context(dbc_path.as_str(), format_spec.as_str(), rules_path.as_str()) {
                Ok(context) => {
                    stop_capture(&flag_for_slcan);
                    *receiver_for_slcan.borrow_mut() = None;
                    *import_for_slcan.borrow_mut() = None;
                    let (sender, rx) = mpsc::sync_channel(4_096);
                    let running = Arc::new(AtomicBool::new(true));
                    let worker_running = Arc::clone(&running);
                    let (port, interface) = (port.to_string(), interface.to_string());
                    std::thread::spawn(move || {
                        match SlcanSerialReceive::open(&port, baud, &interface) {
                            Ok(mut source) => {
                                while worker_running.load(Ordering::Relaxed) {
                                    match source.receive(Duration::from_millis(100)) {
                                        Ok(Some(frame)) => {
                                            if sender.send(Ok(frame)).is_err() { break; }
                                        }
                                        Ok(None) => {}
                                        Err(error) => {
                                            let _ = sender.send(Err(error));
                                            break;
                                        }
                                    }
                                }
                            }
                            Err(error) => { let _ = sender.send(Err(error)); }
                        }
                    });
                    *flag_for_slcan.borrow_mut() = Some(running);
                    *receiver_for_slcan.borrow_mut() = Some(rx);
                    *state_for_slcan.borrow_mut() = ViewerState {
                        frames: Vec::new(), context, live: true, history_dropped: 0, page_offset: 0, ..Default::default()
                    };
                    model_for_slcan.clear();
                    ui.set_show_isotp(false);
                    ui.set_show_stats(false);
                        ui.set_show_bam(false);
                    ui.set_show_bytes(false);
                    ui.set_show_plot(false);
                    ui.set_page_text("Live: latest 2,000 matching rows".into());
                    ui.set_status_text(
                        "Opening passive SLCAN serial • preconfigured/auto-streaming adapter required".into()
                    );
                }
                Err(error) => {
                    ui.set_status_text(format!("Cannot start SLCAN listener: {error}").into())
                }
            }
        }
    });


    // Passive GVRET binary stream. Many devices need a binary-mode
    // handshake, which is intentionally not sent in this adapter.
    let weak = ui.as_weak();
    let state_for_gvret = Rc::clone(&state);
    let model_for_gvret = Rc::clone(&model);
    let flag_for_gvret = Rc::clone(&capture_flag);
    let receiver_for_gvret = Rc::clone(&receiver);
    let import_for_gvret = Rc::clone(&import_rx);
    ui.on_start_gvret_serial(move |port, baud_text, dbc_path, format_spec, rules_path| {
        if let Some(ui) = weak.upgrade() {
            let baud = match baud_text.trim().parse::<u32>() {
                Ok(value) if (9_600..=3_000_000).contains(&value) => value,
                _ => {
                    ui.set_status_text("GVRET serial baud must be 9600–3000000.".into());
                    return;
                }
            };
            if port.trim().is_empty() {
                ui.set_status_text("Enter a GVRET serial path first.".into());
                return;
            }
            match decode_context(dbc_path.as_str(), format_spec.as_str(), rules_path.as_str()) {
                Ok(context) => {
                    stop_capture(&flag_for_gvret);
                    *receiver_for_gvret.borrow_mut() = None;
                    *import_for_gvret.borrow_mut() = None;
                    let (sender, rx) = mpsc::sync_channel(4_096);
                    let running = Arc::new(AtomicBool::new(true));
                    let worker_running = Arc::clone(&running);
                    let path = port.to_string();
                    std::thread::spawn(move || {
                        match GvretSerialReceive::open(&path, baud) {
                            Ok(mut source) => {
                                while worker_running.load(Ordering::Relaxed) {
                                    match source.receive(Duration::from_millis(100)) {
                                        Ok(Some(frame)) => {
                                            if sender.send(Ok(frame)).is_err() { break; }
                                        }
                                        Ok(None) => {}
                                        Err(error) => {
                                            let _ = sender.send(Err(error));
                                            break;
                                        }
                                    }
                                }
                            }
                            Err(error) => { let _ = sender.send(Err(error)); }
                        }
                    });
                    *flag_for_gvret.borrow_mut() = Some(running);
                    *receiver_for_gvret.borrow_mut() = Some(rx);
                    *state_for_gvret.borrow_mut() = ViewerState {
                        frames: Vec::new(), context, live: true, history_dropped: 0, page_offset: 0, ..Default::default()
                    };
                    model_for_gvret.clear();
                    ui.set_show_isotp(false);
                    ui.set_show_stats(false);
                    ui.set_show_bam(false);
                    ui.set_show_bytes(false);
                    ui.set_show_plot(false);
                    ui.set_page_text("Live: latest 2,000 matching rows".into());
                    ui.set_status_text(
                        "Opening passive GVRET serial • device must already emit binary F1 frames".into()
                    );
                }
                Err(error) =>
                    ui.set_status_text(format!("Cannot start GVRET listener: {error}").into()),
            }
        }
    });

    let weak = ui.as_weak();
    let flag_for_stop = Rc::clone(&capture_flag);
    let receiver_for_stop = Rc::clone(&receiver);
    let state_for_stop = Rc::clone(&state);
    let model_for_stop = Rc::clone(&model);
    let filter_for_stop = Rc::clone(&filter);
    ui.on_stop_live(move || {
        stop_capture(&flag_for_stop);
        *receiver_for_stop.borrow_mut() = None;
        let mut state = state_for_stop.borrow_mut();
        state.live = false;
        state.page_offset = 0;
        if let Some(ui) = weak.upgrade() {
            let filter = filter_for_stop.borrow();
            model_for_stop.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
            ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
            ui.set_status_text(format!("Capture stopped • {} frames retained",
                state.frames.len()).into());
        }
    });

    let weak = ui.as_weak();
    let receiver_for_timer = Rc::clone(&receiver);
    let state_for_timer = Rc::clone(&state);
    let model_for_timer = Rc::clone(&model);
    let flag_for_timer = Rc::clone(&capture_flag);
    let filter_for_timer = Rc::clone(&filter);
    let import_for_timer = Rc::clone(&import_rx);
    let timer = Timer::default();
    timer.start(TimerMode::Repeated, Duration::from_millis(100), move || {
        let Some(ui) = weak.upgrade() else { return; };
        let pending_import = {
            let guard = import_for_timer.borrow();
            guard.as_ref().map(|rx| rx.try_recv())
        };
        match pending_import {
            Some(Ok(Ok((new_state, message)))) => {
                *import_for_timer.borrow_mut() = None;
                stop_capture(&flag_for_timer);
                *receiver_for_timer.borrow_mut() = None;
                let filter = filter_for_timer.borrow();
                model_for_timer.set_vec(visible_rows(&new_state, &filter, OFFLINE_ROWS));
                ui.set_page_text(offline_page_text(&new_state, &filter, OFFLINE_ROWS).into());
                *state_for_timer.borrow_mut() = new_state;
                ui.set_show_isotp(false);
                ui.set_show_stats(false);
                ui.set_show_bam(false);
                    ui.set_show_bytes(false);
                    ui.set_show_plot(false);
                ui.set_status_text(message.into());
            }
            Some(Ok(Err(message))) => {
                *import_for_timer.borrow_mut() = None;
                ui.set_status_text(format!("Could not import trace: {message}").into());
            }
            Some(Err(mpsc::TryRecvError::Disconnected)) => {
                *import_for_timer.borrow_mut() = None;
                ui.set_status_text("Import worker exited without a result.".into());
            }
            _ => {}
        }
        let mut received = 0usize;
        let mut error_text = None;
        let mut disconnected = false;
        {
            let guard = receiver_for_timer.borrow();
            if let Some(rx) = guard.as_ref() {
                for _ in 0..2048 {
                    match rx.try_recv() {
                        Ok(Ok(frame)) => {
                            let mut state = state_for_timer.borrow_mut();
                            state.frames.push(frame);
                            received += 1;
                        }
                        Ok(Err(error)) => {
                            error_text = Some(error.to_string());
                            break;
                        }
                        Err(mpsc::TryRecvError::Disconnected) => {
                            disconnected = true;
                            break;
                        }
                        Err(mpsc::TryRecvError::Empty) => break,
                    }
                }
            }
        }
        if error_text.is_some() || disconnected {
            stop_capture(&flag_for_timer);
            *receiver_for_timer.borrow_mut() = None;
            state_for_timer.borrow_mut().live = false;
        }
        if received > 0 {
            let mut state = state_for_timer.borrow_mut();
            if state.frames.len() > LIVE_HISTORY_LIMIT {
                let overflow = state.frames.len() - LIVE_HISTORY_LIMIT;
                state.frames.drain(..overflow);
                state.history_dropped += overflow;
            }
            let filter = filter_for_timer.borrow();
            let matched = filtered_count(&state, &filter);
            model_for_timer.set_vec(visible_rows(&state, &filter, LIVE_ROWS));
            ui.set_status_text(format!("Receiving • {} buffered • {matched} match • {} older frames discarded • {} shown",
                state.frames.len(), state.history_dropped, model_for_timer.row_count()).into());
        }
        {
            let mut state = state_for_timer.borrow_mut();
            if state.replay_playing && !state.live {
                let now = Instant::now();
                let elapsed = state.replay_last_tick.replace(now)
                    .map(|previous| now.saturating_duration_since(previous).as_micros())
                    .unwrap_or(0).min(u64::MAX as u128) as u64;
                let simulated_delta = (elapsed as f64 * state.replay_speed_factor)
                    .clamp(0.0, u64::MAX as f64) as u64;
                let (newly_visible, finished) = state.replay.as_mut().map(|replay| {
                    let count = replay.advance(simulated_delta);
                    (count, replay.finished())
                }).unwrap_or((0, true));
                if newly_visible > 0 || finished {
                    let filter = filter_for_timer.borrow();
                    model_for_timer.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
                    ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
                }
                if finished {
                    state.replay_playing = false;
                    state.replay_last_tick = None;
                    ui.set_status_text("Offline replay complete; use Play to restart.".into());
                }
            }
        }
        if let Some(error) = error_text {
            ui.set_status_text(format!("Live capture stopped: {error}").into());
        } else if disconnected {
            ui.set_status_text("Live source disconnected.".into());
        }
    });

    if let Some(path) = std::env::args().nth(1) {
        let dbc = std::env::args().nth(2).unwrap_or_default();
        let format_spec = std::env::args().nth(3).unwrap_or_default();
        let rules_path = std::env::args().nth(4).unwrap_or_default();
        ui.set_file_path(path.clone().into());
        ui.set_dbc_path(dbc.clone().into());
        ui.set_format_spec(format_spec.clone().into());
        ui.set_rules_path(rules_path.clone().into());
        let (sender, rx) = mpsc::sync_channel(1);
        *import_rx.borrow_mut() = Some(rx);
        std::thread::spawn(move || {
            let result = open_trace(&path, &dbc, &format_spec, &rules_path)
                .map_err(|error| error.to_string());
            let _ = sender.send(result);
        });
        ui.set_status_text("Loading startup CAN trace in background…".into());
    }
    ui.run()?;
    stop_capture(&capture_flag);
    Ok(())
}

#[cfg(test)]
mod export_view_tests {
    use super::*;
    use savvycan_core::{CanId, FdFlags};

    fn frame(us: u64, id: u16, value: u8) -> CanFrame {
        CanFrame::new(Some(us), "can0", CanId::Standard(id),
            FrameKind::Data, vec![value], false, FdFlags::default()).unwrap()
    }

    #[test]
    fn empty_filter_borrows_all_retained_frames() {
        let frames = vec![frame(1, 0x123, 0x11), frame(2, 0x456, 0x22)];
        assert!(matches!(selected_export_frames(&frames, None),
            std::borrow::Cow::Borrowed(_)));
        assert_eq!(selected_export_frames(&frames, None).len(), 2);
        let all = TraceFilter::default();
        assert!(matches!(selected_export_frames(&frames, Some(&all)),
            std::borrow::Cow::Borrowed(_)));
    }

    #[test]
    fn filtered_export_uses_all_matches_not_only_visible_page() {
        let frames = vec![
            frame(1_000_000, 0x123, 0x11),
            frame(2_000_000, 0x456, 0x22),
            frame(3_000_000, 0x123, 0x33),
            frame(4_000_000, 0x123, 0x44),
        ];
        let query = TraceFilter::parse_with_time_and_payload(
            "can0", "S:123", "2", "4", "33").unwrap();
        let matching = selected_export_frames(&frames, Some(&query));
        assert_eq!(matching.len(), 1);
        assert_eq!(matching[0].data, vec![0x33]);
        let none = TraceFilter::parse_with_time("", "S:777", "", "").unwrap();
        assert!(selected_export_frames(&frames, Some(&none)).is_empty());
    }
    #[test]
    fn relative_plot_windows_use_the_correct_time_base() {
        assert_eq!(plot_window_bounds(5_000_000, 10_000_000,
            HorizontalAxis::TimestampMicroseconds, "1.25", "3").unwrap(),
            (6_250_000, 8_000_000));
        assert_eq!(plot_window_bounds(50, 80,
            HorizontalAxis::FrameIndex, "2", "20").unwrap(), (52, 70));
        assert_eq!(plot_window_bounds(50, 80,
            HorizontalAxis::FrameIndex, "", "").unwrap(), (50, 80));
        assert!(plot_window_bounds(50, 80,
            HorizontalAxis::FrameIndex, "2.5", "10").is_err());
        assert!(plot_window_bounds(50, 80,
            HorizontalAxis::FrameIndex, "30", "10").is_err());
        assert!(plot_window_bounds(50, 80,
            HorizontalAxis::FrameIndex, "NaN", "").is_err());
    }

    #[test]
    fn plot_navigation_clamps_and_zooms_around_the_window_centre() {
        let mut window = PlotNavigation {
            first: 0, last: 1_000, start: 200, end: 600,
            axis: HorizontalAxis::TimestampMicroseconds,
        };
        assert_eq!(navigate_plot_window(window, "in").unwrap(), (300, 500));
        assert_eq!(navigate_plot_window(window, "out").unwrap(), (0, 800));
        assert_eq!(navigate_plot_window(window, "right").unwrap(), (300, 700));
        assert_eq!(navigate_plot_window(window, "left").unwrap(), (100, 500));
        assert_eq!(navigate_plot_window(window, "fit").unwrap(), (0, 1_000));
        window.start = 900;
        window.end = 1_000;
        assert_eq!(navigate_plot_window(window, "right").unwrap(), (900, 1_000));
        window.start = 0;
        window.end = 100;
        assert_eq!(navigate_plot_window(window, "left").unwrap(), (0, 100));
        assert!(navigate_plot_window(window, "unknown").is_err());
    }

    #[test]
    fn wheel_zoom_preserves_pointer_anchor_and_clamps_at_edges() {
        let window = PlotNavigation {
            first: 1_700_000_000_000_000, last: 1_700_000_000_010_000,
            start: 1_700_000_000_002_000, end: 1_700_000_000_006_000,
            axis: HorizontalAxis::TimestampMicroseconds,
        };
        let (start, end) = zoom_plot_at(window, -120.0, 0.25);
        assert!(start > window.start && end < window.end);
        let before_anchor = window.start - window.first + 1_000;
        let after_anchor = start - window.first + ((end - start) as f64 * 0.25).round() as u64;
        assert!(before_anchor.abs_diff(after_anchor) <= 1);
        assert_eq!(zoom_plot_at(window, 1e8, 0.5), (window.first, window.last));
        assert_eq!(zoom_plot_at(window, f64::NAN, 0.5), (window.start, window.end));
        let edge = PlotNavigation { start: window.first, end: window.first + 2_000, ..window };
        assert_eq!(pan_plot_by_fraction(edge, 0.9), (edge.start, edge.end));
        assert_eq!(pan_plot_by_fraction(edge, -0.5), (edge.start + 1_000, edge.end + 1_000));
    }

    #[test]
    fn cursor_uses_nearest_visible_samples_on_sorted_sparse_series() {
        let session = PlotSession {
            series: vec![NumericSeries {
                signal: "RPM".into(), unit: "rpm".into(),
                axis: HorizontalAxis::FrameIndex,
                points: vec![(10, 100.0), (20, 200.0), (30, 300.0)],
                decode_errors: 0,
            }],
            first: 10, last: 30, axis: HorizontalAxis::FrameIndex, independent: true,
        };
        let window = PlotNavigation {
            first: 10, last: 30, start: 14, end: 25,
            axis: HorizontalAxis::FrameIndex,
        };
        assert!(plot_cursor_readout(&session, window, 0.0).contains("RPM: 200.000 rpm"));
        assert!(plot_cursor_readout(&session, window, 1.0).contains("RPM: 200.000 rpm"));
        let empty = PlotNavigation { start: 21, end: 25, ..window };
        assert!(plot_cursor_readout(&session, empty, 0.5).contains("RPM: —"));
        assert!(plot_cursor_readout(&session, window, -1.0).starts_with("Hover"));
    }

    #[test]
    fn plot_navigation_offsets_preserve_microsecond_precision() {
        assert_eq!(plot_relative_offset(6_234_567, 5_000_000,
            HorizontalAxis::TimestampMicroseconds), "1.234567");
        assert_eq!(plot_relative_offset(54, 50, HorizontalAxis::FrameIndex), "4");
        let navigation = PlotNavigation {
            first: 10, last: 10, start: 10, end: 11, axis: HorizontalAxis::FrameIndex,
        };
        assert_eq!(navigate_plot_window(navigation, "fit").unwrap(), (10, 11));
    }

    #[test]
    fn offline_replay_displays_only_revealed_frames_and_restores_full_view() {
        let frames = vec![frame(1_000_000, 0x123, 0x11),
            frame(1_100_000, 0x456, 0x22), frame(1_200_000, 0x123, 0x33)];
        let filter = TraceFilter::parse("", "S:123").unwrap();
        let mut state = ViewerState { frames, ..Default::default() };
        assert_eq!(visible_rows(&state, &filter, 100).len(), 2);
        state.replay = Some(OfflineReplay::new(&state.frames, &filter));
        assert!(visible_rows(&state, &filter, 100).is_empty());
        state.replay.as_mut().unwrap().step_frames(1);
        assert_eq!(visible_rows(&state, &filter, 100).len(), 1);
        assert!(offline_page_text(&state, &filter, 100).contains("1 / 2 frames"));
        state.replay.as_mut().unwrap().step_frames(1);
        assert_eq!(visible_rows(&state, &filter, 100).len(), 2);
        state.replay = None;
        assert_eq!(visible_rows(&state, &filter, 100).len(), 2);
        assert!(offline_page_text(&state, &filter, 100).contains("matching"));
    }

}
