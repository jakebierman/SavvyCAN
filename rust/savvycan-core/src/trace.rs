use std::collections::HashSet;
use crate::{CanFrame, CanId, FrameKind};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TraceSummary {
    pub frames: usize,
    pub unique_identifiers: usize,
    pub classic_frames: usize,
    pub fd_frames: usize,
    pub remote_requests: usize,
    pub timestamped_frames: usize,
}

/// Group identifiers by interface AND identifier format (11/29-bit).
pub fn summarize(frames: &[CanFrame]) -> TraceSummary {
    let unique: HashSet<(&str, CanId)> = frames.iter()
        .map(|frame| (frame.interface.as_str(), frame.id))
        .collect();
    let mut summary = TraceSummary {
        frames: frames.len(),
        unique_identifiers: unique.len(),
        ..TraceSummary::default()
    };
    for frame in frames {
        if frame.is_fd {
            summary.fd_frames += 1;
        } else {
            summary.classic_frames += 1;
        }
        if matches!(frame.kind, FrameKind::RemoteRequest { .. }) {
            summary.remote_requests += 1;
        }
        if frame.timestamp_us.is_some() {
            summary.timestamped_frames += 1;
        }
    }
    summary
}
