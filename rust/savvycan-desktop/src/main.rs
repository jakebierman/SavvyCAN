use std::cell::RefCell;
use std::collections::VecDeque;
use std::error::Error;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::Duration;

use savvycan_core::{
    parse_format_rules, read_trace, summarize, write_candump, write_crtd, write_decoded_csv, write_gvret_csv, write_id_stats_csv, write_socketcan_pcap, write_socketcan_pcapng,
    analyze_ids, describe_uds_like_payload, CanFrame, DbcDatabase, DbcStack, DecodedSignal,
    CanReceive, FrameKind, IdFilter, IsoTpAssembler, J1939BamAssembler, PayloadFormatter,
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
        format: format.into(),
        length: frame.len().to_string().into(),
        payload: payload.into(),
        message_name: message_name.into(),
        decoded_values: decoded_values.into(),
        typed_values: typed_values.into(),
    }
}

fn visible_rows(state: &ViewerState, filter: &TraceFilter, limit: usize) -> Vec<FrameRow> {
    if state.live {
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
    Ok((ViewerState { frames, context, live: false, history_dropped: 0, page_offset: 0 }, message))
}

fn export_trace(path: &str, state: &ViewerState) -> Result<Option<usize>, Box<dyn Error>> {
    if state.frames.is_empty() { return Err("Load a non-empty trace before exporting".into()); }
    if path.trim().is_empty() { return Err("Enter an export path".into()); }
    let mut bytes = Vec::new();
    let lower = path.to_ascii_lowercase();
    let signal_count = if lower.ends_with(".stats.csv") {
        Some(write_id_stats_csv(&mut bytes, &state.frames)?)
    } else if lower.ends_with(".signals.csv") {
        let dbc = state.context.dbc.as_ref()
            .ok_or("Load at least one DBC before exporting decoded signals")?;
        Some(write_decoded_csv(&mut bytes, &state.frames, dbc)?)
    } else if lower.ends_with(".crtd") || lower.ends_with(".crt") {
        write_crtd(&mut bytes, &state.frames)?;
        None
    } else if lower.ends_with(".csv") {
        write_gvret_csv(&mut bytes, &state.frames)?;
        None
    } else if lower.ends_with(".log") || lower.ends_with(".candump") {
        write_candump(&mut bytes, &state.frames)?;
        None
    } else if lower.ends_with(".pcapng") {
        write_socketcan_pcapng(&mut bytes, &state.frames)?;
        None
    } else if lower.ends_with(".pcap") {
        write_socketcan_pcap(&mut bytes, &state.frames)?;
        None
    } else {
        return Err("Use .crt/.crtd, .csv, .log, .candump, .pcap, .pcapng, .stats.csv or .signals.csv for export".into());
    };
    // No overwrite and no partial output on format-validation failure.
    let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(&bytes)?;
    Ok(signal_count)
}

fn stop_capture(flag: &Rc<RefCell<Option<Arc<AtomicBool>>>>) {
    if let Some(running) = flag.borrow_mut().take() {
        running.store(false, Ordering::Relaxed);
    }
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
    ui.on_export_trace(move |path| {
        if let Some(ui) = weak.upgrade() {
            let state = state_for_export.borrow();
            match export_trace(path.as_str(), &state) {
                Ok(Some(rows)) => {
                    let kind = if path.to_ascii_lowercase().ends_with(".stats.csv") {
                        "CAN interface/ID statistics groups"
                    } else { "decoded DBC signal values" };
                    ui.set_status_text(format!("Exported {rows} {kind} to {path}").into());
                },
                Ok(None) => {
                    let candump = path.to_ascii_lowercase().ends_with(".log")
                        || path.to_ascii_lowercase().ends_with(".candump");
                    let caveat = if candump { " • candump does not store Rx/Tx direction" }
                        else if path.to_ascii_lowercase().ends_with(".pcap") {
                            " • PCAP omits interface and Rx/Tx direction"
                        } else { "" };
                    ui.set_status_text(format!("Exported {} frames to {path}{caveat}",
                        state.frames.len()).into());
                },
                Err(error) => ui.set_status_text(format!("Could not export: {error}").into()),
            }
        }
    });

    let weak = ui.as_weak();
    let state_for_filter = Rc::clone(&state);
    let model_for_filter = Rc::clone(&model);
    let filter_for_apply = Rc::clone(&filter);
    ui.on_apply_filter(move |interface, id, from, through| {
        if let Some(ui) = weak.upgrade() {
            match TraceFilter::parse_with_time(interface.as_str(), id.as_str(),
                from.as_str(), through.as_str()) {
                Ok(new_filter) => {
                    let mut state = state_for_filter.borrow_mut();
                    state.page_offset = 0;
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
            let filter = filter_for_next.borrow();
            let next = state.page_offset.saturating_add(OFFLINE_ROWS);
            if next < filtered_count(&state, &filter) {
                state.page_offset = next;
            }
            model_for_next.set_vec(visible_rows(&state, &filter, OFFLINE_ROWS));
            ui.set_page_text(offline_page_text(&state, &filter, OFFLINE_ROWS).into());
        }
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
            ui.set_status_text(message.into());
        }
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
                            page_offset: 0,
                        };
                        model_for_start.clear();
                        ui.set_show_isotp(false);
                        ui.set_show_stats(false);
                        ui.set_show_bam(false);
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
                        frames: Vec::new(), context, live: true, history_dropped: 0, page_offset: 0,
                    };
                    model_for_slcan.clear();
                    ui.set_show_isotp(false);
                    ui.set_show_stats(false);
                        ui.set_show_bam(false);
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
                        frames: Vec::new(), context, live: true, history_dropped: 0, page_offset: 0,
                    };
                    model_for_gvret.clear();
                    ui.set_show_isotp(false);
                    ui.set_show_stats(false);
                    ui.set_show_bam(false);
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
