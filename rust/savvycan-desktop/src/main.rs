use std::error::Error;
use std::cell::RefCell;
use std::fs::{File, OpenOptions};
use std::io::{BufReader, Write};
use std::rc::Rc;

use savvycan_core::{
    parse_format_rules, read_trace, summarize, write_crtd, write_gvret_csv, CanFrame,
    DbcDatabase, DecodedSignal, FrameKind, PayloadFormatter, PayloadFormatRouter,
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

fn row(frame: CanFrame, dbc: Option<&DbcDatabase>, formatter: Option<&PayloadFormatter>, router: Option<&PayloadFormatRouter>) -> FrameRow {
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
        router.and_then(|r| r.formatter_for(&frame.interface, frame.id))
            .or(formatter).map(|f| f.format(&frame.data)).unwrap_or_default()
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

fn open_trace(ui: &AppWindow, path: &str, dbc_path: &str, format_spec: &str, rules_path: &str) -> Result<Vec<CanFrame>, Box<dyn Error>> {
    let rules = if rules_path.trim().is_empty() {
        None
    } else {
        Some(parse_format_rules(&std::fs::read_to_string(rules_path)?)?)
    };
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
    let imported = read_trace(reader)?;
    let format = imported.format;
    let frames = imported.frames;
    let summary = summarize(&frames);

    const ROW_LIMIT: usize = 25_000;
    let rows = frames.iter().take(ROW_LIMIT).cloned()
        .map(|frame| row(frame, dbc.as_ref(), formatter.as_ref(), rules.as_ref()))
        .collect::<Vec<_>>();
    ui.set_frames(ModelRc::new(VecModel::from(rows)));
    ui.set_status_text(format!(
        "{} frames • {} distinct interface/ID pairs • {} classic • {} FD • {} RTR • {format}{}{}{}{}",
        summary.frames,
        summary.unique_identifiers,
        summary.classic_frames,
        summary.fd_frames,
        summary.remote_requests,
        dbc.as_ref().map(|db| format!(" • DBC: {} messages", db.messages.len())).unwrap_or_default(),
        if formatter.is_some() { " • default typed formatter" } else { "" },
        if rules.is_some() { " • per-ID rules active" } else { "" },
        if summary.frames > ROW_LIMIT { " • first 25,000 displayed" } else { "" },
    ).into());
    Ok(frames)
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
    // Refuse to overwrite an existing capture. Validate format before file creation.
    let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(&bytes)?;
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let ui = AppWindow::new()?;
    let loaded = Rc::new(RefCell::new(Vec::<CanFrame>::new()));
    let weak = ui.as_weak();
    let loaded_for_open = Rc::clone(&loaded);
    ui.on_open_trace(move |path, dbc_path, format_spec, rules_path| {
        if let Some(ui) = weak.upgrade() {
            if path.trim().is_empty() {
                ui.set_status_text("Enter a CAN trace path first.".into());
            } else {
                match open_trace(&ui, path.as_str(), dbc_path.as_str(),
                    format_spec.as_str(), rules_path.as_str()) {
                    Ok(frames) => *loaded_for_open.borrow_mut() = frames,
                    Err(error) => ui.set_status_text(format!("Could not import trace: {error}").into()),
                }
            }
        }
    });

    let weak = ui.as_weak();
    let loaded_for_export = Rc::clone(&loaded);
    ui.on_export_trace(move |path| {
        if let Some(ui) = weak.upgrade() {
            match export_trace(path.as_str(), &loaded_for_export.borrow()) {
                Ok(()) => ui.set_status_text(format!("Exported {} frames to {path}",
                    loaded_for_export.borrow().len()).into()),
                Err(error) => ui.set_status_text(format!("Could not export: {error}").into()),
            }
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
        match open_trace(&ui, &path, &dbc, &format_spec, &rules_path) {
            Ok(frames) => *loaded.borrow_mut() = frames,
            Err(error) => ui.set_status_text(format!("Could not import trace: {error}").into()),
        }
    }

    ui.run()?;
    Ok(())
}
