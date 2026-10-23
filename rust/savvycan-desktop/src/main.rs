use std::cell::RefCell;
use std::error::Error;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::Duration;

use savvycan_core::{
    parse_format_rules, read_trace, summarize, write_crtd, write_gvret_csv,
    CanFrame, DbcDatabase, DecodedSignal, FrameKind, PayloadFormatter,
    PayloadFormatRouter, ReceiveError,
};
use slint::{ModelRc, VecModel, Timer, TimerMode};

#[cfg(target_os = "linux")]
use savvycan_core::CanReceive;
#[cfg(target_os = "linux")]
use savvycan_transports::SocketCanReceive;

slint::include_modules!();

#[derive(Default)]
struct DecodeContext {
    dbc: Option<DbcDatabase>,
    formatter: Option<PayloadFormatter>,
    rules: Option<PayloadFormatRouter>,
}

#[derive(Default)]
struct ViewerState {
    frames: Vec<CanFrame>,
    context: DecodeContext,
    live: bool,
    history_dropped: usize,
}

fn decode_context(dbc_path: &str, format_spec: &str, rules_path: &str)
    -> Result<DecodeContext, Box<dyn Error>>
{
    let dbc = if dbc_path.trim().is_empty() { None }
        else { Some(DbcDatabase::parse(&std::fs::read_to_string(dbc_path)?)?) };
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
        if context.dbc.is_some() { " • DBC loaded" } else { "" },
        if context.formatter.is_some() { " • default formatter" } else { "" },
        if context.rules.is_some() { " • per-ID rules" } else { "" },
    );
    Ok((ViewerState { frames, context, live: false, history_dropped: 0 }, message))
}

fn export_trace(path: &str, frames: &[CanFrame]) -> Result<(), Box<dyn Error>> {
    if frames.is_empty() { return Err("Load a non-empty trace before exporting".into()); }
    if path.trim().is_empty() { return Err("Enter an export path".into()); }
    let mut bytes = Vec::new();
    let lower = path.to_ascii_lowercase();
    if lower.ends_with(".crtd") || lower.ends_with(".crt") {
        write_crtd(&mut bytes, frames)?;
    } else if lower.ends_with(".csv") {
        write_gvret_csv(&mut bytes, frames)?;
    } else {
        return Err("Use .crt/.crtd for CRTD or .csv for GVRET CSV export".into());
    }
    // No overwrite and no partial output on format-validation failure.
    let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(&bytes)?;
    Ok(())
}

fn stop_capture(flag: &Rc<RefCell<Option<Arc<AtomicBool>>>>) {
    if let Some(running) = flag.borrow_mut().take() {
        running.store(false, Ordering::Relaxed);
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    const OFFLINE_ROWS: usize = 25_000;
    const LIVE_ROWS: usize = 2_000;
    const LIVE_HISTORY_LIMIT: usize = 100_000;

    let ui = AppWindow::new()?;
    let model = Rc::new(VecModel::<FrameRow>::from(Vec::new()));
    ui.set_frames(ModelRc::from(Rc::clone(&model)));
    let state = Rc::new(RefCell::new(ViewerState::default()));
    let capture_flag: Rc<RefCell<Option<Arc<AtomicBool>>>> = Rc::new(RefCell::new(None));
    let receiver: Rc<RefCell<Option<mpsc::Receiver<Result<CanFrame, ReceiveError>>>>> =
        Rc::new(RefCell::new(None));

    let weak = ui.as_weak();
    let state_for_open = Rc::clone(&state);
    let model_for_open = Rc::clone(&model);
    let flag_for_open = Rc::clone(&capture_flag);
    let receiver_for_open = Rc::clone(&receiver);
    ui.on_open_trace(move |path, dbc_path, format_spec, rules_path| {
        if let Some(ui) = weak.upgrade() {
            if path.trim().is_empty() {
                ui.set_status_text("Enter a CAN trace path first.".into());
            } else {
                match open_trace(path.as_str(), dbc_path.as_str(),
                    format_spec.as_str(), rules_path.as_str()) {
                    Ok((new_state, message)) => {
                        stop_capture(&flag_for_open);
                        *receiver_for_open.borrow_mut() = None;
                        let rows = new_state.frames.iter().take(OFFLINE_ROWS)
                            .map(|frame| row(frame, &new_state.context)).collect::<Vec<_>>();
                        model_for_open.set_vec(rows);
                        let truncated = new_state.frames.len() > OFFLINE_ROWS;
                        *state_for_open.borrow_mut() = new_state;
                        ui.set_status_text(format!("{message}{}",
                            if truncated { " • first 25,000 displayed" } else { "" }).into());
                    }
                    Err(error) => ui.set_status_text(format!("Could not import trace: {error}").into()),
                }
            }
        }
    });

    let weak = ui.as_weak();
    let state_for_export = Rc::clone(&state);
    ui.on_export_trace(move |path| {
        if let Some(ui) = weak.upgrade() {
            let state = state_for_export.borrow();
            match export_trace(path.as_str(), &state.frames) {
                Ok(()) => ui.set_status_text(format!("Exported {} frames to {path}",
                    state.frames.len()).into()),
                Err(error) => ui.set_status_text(format!("Could not export: {error}").into()),
            }
        }
    });

    let weak = ui.as_weak();
    let state_for_start = Rc::clone(&state);
    let model_for_start = Rc::clone(&model);
    let flag_for_start = Rc::clone(&capture_flag);
    let receiver_for_start = Rc::clone(&receiver);
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
                        };
                        model_for_start.clear();
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
    let flag_for_stop = Rc::clone(&capture_flag);
    let receiver_for_stop = Rc::clone(&receiver);
    let state_for_stop = Rc::clone(&state);
    ui.on_stop_live(move || {
        stop_capture(&flag_for_stop);
        *receiver_for_stop.borrow_mut() = None;
        state_for_stop.borrow_mut().live = false;
        if let Some(ui) = weak.upgrade() {
            ui.set_status_text(format!("Capture stopped • {} frames retained",
                state_for_stop.borrow().frames.len()).into());
        }
    });

    let weak = ui.as_weak();
    let receiver_for_timer = Rc::clone(&receiver);
    let state_for_timer = Rc::clone(&state);
    let model_for_timer = Rc::clone(&model);
    let flag_for_timer = Rc::clone(&capture_flag);
    let timer = Timer::default();
    timer.start(TimerMode::Repeated, Duration::from_millis(100), move || {
        let Some(ui) = weak.upgrade() else { return; };
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
            let start = state.frames.len().saturating_sub(LIVE_ROWS);
            let rows = state.frames[start..].iter()
                .map(|frame| row(frame, &state.context)).collect::<Vec<_>>();
            model_for_timer.set_vec(rows);
            ui.set_status_text(format!("Receiving • {} buffered • {} older frames discarded • showing latest {}",
                state.frames.len(), state.history_dropped, LIVE_ROWS.min(state.frames.len())).into());
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
        match open_trace(&path, &dbc, &format_spec, &rules_path) {
            Ok((new_state, message)) => {
                model.set_vec(new_state.frames.iter().take(OFFLINE_ROWS)
                    .map(|frame| row(frame, &new_state.context)).collect::<Vec<_>>());
                *state.borrow_mut() = new_state;
                ui.set_status_text(message.into());
            }
            Err(error) => ui.set_status_text(format!("Could not import trace: {error}").into()),
        }
    }
    ui.run()?;
    stop_capture(&capture_flag);
    Ok(())
}
