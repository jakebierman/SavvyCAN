use std::error::Error;
use std::fs::File;
use std::io::BufReader;

use savvycan_core::{
    read_candump, summarize, CanFrame, DbcDatabase, DecodedSignal, FrameKind, PayloadFormatter,
};
use slint::{ModelRc, VecModel};

slint::include_modules!();

fn signal_text(signal: DecodedSignal) -> String {
    let value = signal.text.unwrap_or_else(|| signal.value.to_string());
    if signal.unit.is_empty() {
        format!("{}={value}", signal.name)
    } else {
        format!("{}={value} {}", signal.name, signal.unit)
    }
}

fn row(frame: CanFrame, dbc: Option<&DbcDatabase>, formatter: Option<&PayloadFormatter>) -> FrameRow {
    let timestamp = frame.timestamp_us.map(|us| {
        format!("{}.{:06}", us / 1_000_000, us % 1_000_000)
    }).unwrap_or_else(|| "—".into());
    let format = match frame.kind {
        FrameKind::RemoteRequest { .. } => "RTR".to_string(),
        FrameKind::Data if frame.is_fd => {
            match (frame.fd_flags.bitrate_switch, frame.fd_flags.error_state_indicator) {
                (true, true) => "FD/BRS/ESI",
                (true, false) => "FD/BRS",
                (false, true) => "FD/ESI",
                (false, false) => "FD",
            }.to_string()
        }
        FrameKind::Data => "CAN".to_string(),
    };
    let payload = frame.data.iter()
        .map(|byte| format!("{byte:02X}"))
        .collect::<Vec<_>>()
        .join(" ");
    let (message_name, decoded_values) = match dbc.map(|db| db.decode(&frame)) {
        Some(Ok(Some(decoded))) => (
            decoded.name,
            decoded.signals.into_iter().map(signal_text).collect::<Vec<_>>().join("  |  "),
        ),
        Some(Err(error)) => (String::new(), format!("Decode error: {error}")),
        _ => (String::new(), String::new()),
    };
    let typed_values = if matches!(frame.kind, FrameKind::Data) {
        formatter.map(|f| f.format(&frame.data)).unwrap_or_default()
    } else { String::new() };
    let length = frame.len();
    FrameRow {
        timestamp: timestamp.into(),
        channel: frame.interface.into(),
        can_id: frame.id.to_string().into(),
        format: format.into(),
        length: length.to_string().into(),
        payload: payload.into(),
        message_name: message_name.into(),
        decoded_values: decoded_values.into(),
        typed_values: typed_values.into(),
    }
}

fn open_trace(ui: &AppWindow, path: &str, dbc_path: &str, format_spec: &str) -> Result<(), Box<dyn Error>> {
    let formatter = if format_spec.trim().is_empty() {
        None
    } else {
        Some(PayloadFormatter::compile(format_spec, false)?)
    };
    let dbc = if dbc_path.trim().is_empty() {
        None
    } else {
        Some(DbcDatabase::parse(&std::fs::read_to_string(dbc_path)?)?)
    };
    let reader = BufReader::new(File::open(path)?);
    let frames = read_candump(reader)?;
    let summary = summarize(&frames);

    const ROW_LIMIT: usize = 25_000;
    let rows = frames.into_iter().take(ROW_LIMIT)
        .map(|frame| row(frame, dbc.as_ref(), formatter.as_ref()))
        .collect::<Vec<_>>();
    ui.set_frames(ModelRc::new(VecModel::from(rows)));
    ui.set_status_text(format!(
        "{} frames • {} distinct interface/ID pairs • {} classic • {} FD • {} RTR{}{}{}",
        summary.frames,
        summary.unique_identifiers,
        summary.classic_frames,
        summary.fd_frames,
        summary.remote_requests,
        dbc.as_ref().map(|db| format!(" • DBC: {} messages", db.messages.len())).unwrap_or_default(),
        if formatter.is_some() { " • typed formatter active" } else { "" },
        if summary.frames > ROW_LIMIT { " • first 25,000 displayed" } else { "" },
    ).into());
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let ui = AppWindow::new()?;
    let weak = ui.as_weak();
    ui.on_open_trace(move |path, dbc_path, format_spec| {
        if let Some(ui) = weak.upgrade() {
            if path.trim().is_empty() {
                ui.set_status_text("Enter a log file path first.".into());
            } else if let Err(error) = open_trace(&ui, path.as_str(), dbc_path.as_str(), format_spec.as_str()) {
                ui.set_status_text(format!("Could not import trace: {error}").into());
            }
        }
    });

    if let Some(path) = std::env::args().nth(1) {
        let dbc = std::env::args().nth(2).unwrap_or_default();
        let format_spec = std::env::args().nth(3).unwrap_or_default();
        ui.set_file_path(path.clone().into());
        ui.set_dbc_path(dbc.clone().into());
        ui.set_format_spec(format_spec.clone().into());
        if let Err(error) = open_trace(&ui, &path, &dbc, &format_spec) {
            ui.set_status_text(format!("Could not import trace: {error}").into());
        }
    }

    ui.run()?;
    Ok(())
}
