use std::error::Error;
use std::fs::File;
use std::io::BufReader;

use savvycan_core::{read_candump, summarize, CanFrame, FrameKind};
use slint::{ModelRc, VecModel};

slint::include_modules!();

fn row(frame: CanFrame) -> FrameRow {
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
    FrameRow {
        timestamp: timestamp.into(),
        channel: frame.interface.into(),
        can_id: frame.id.to_string().into(),
        format: format.into(),
        length: frame.len().to_string().into(),
        payload: payload.into(),
    }
}

fn open_trace(ui: &AppWindow, path: &str) -> Result<(), Box<dyn Error>> {
    let reader = BufReader::new(File::open(path)?);
    let frames = read_candump(reader)?;
    let summary = summarize(&frames);

    // Bounded UI population for the initial preview. Streaming/filtering follows.
    const ROW_LIMIT: usize = 25_000;
    let rows = frames.into_iter().take(ROW_LIMIT).map(row).collect::<Vec<_>>();
    ui.set_frames(ModelRc::new(VecModel::from(rows)));
    ui.set_status_text(format!(
        "{} frames • {} distinct interface/ID pairs • {} classic • {} FD • {} RTR{}",
        summary.frames,
        summary.unique_identifiers,
        summary.classic_frames,
        summary.fd_frames,
        summary.remote_requests,
        if summary.frames > ROW_LIMIT { " • first 25,000 displayed" } else { "" },
    ).into());
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let ui = AppWindow::new()?;
    let weak = ui.as_weak();
    ui.on_open_trace(move |path| {
        if let Some(ui) = weak.upgrade() {
            if path.trim().is_empty() {
                ui.set_status_text("Enter a log file path first.".into());
            } else if let Err(error) = open_trace(&ui, path.as_str()) {
                ui.set_status_text(format!("Could not import trace: {error}").into());
            }
        }
    });

    if let Some(path) = std::env::args().nth(1) {
        ui.set_file_path(path.clone().into());
        if let Err(error) = open_trace(&ui, &path) {
            ui.set_status_text(format!("Could not import trace: {error}").into());
        }
    }

    ui.run()?;
    Ok(())
}
