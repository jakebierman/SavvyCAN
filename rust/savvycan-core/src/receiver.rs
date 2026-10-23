//! Receive-only CAN transport contracts shared by desktop and future adapters.
//! Transmit/diagnostic control is deliberately not exposed by this API.
use crate::CanFrame;
use std::collections::VecDeque;
use std::fmt;
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ReceiveCapabilities {
    pub classic_can: bool,
    pub can_fd: bool,
    pub rtr: bool,
    pub hardware_timestamps: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReceiveDescriptor {
    pub name: String,
    pub interface: String,
    pub capabilities: ReceiveCapabilities,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ReceiveError {
    Io(String),
    Protocol(String),
    Disconnected,
}
impl fmt::Display for ReceiveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(s) => write!(f, "receive I/O error: {s}"),
            Self::Protocol(s) => write!(f, "CAN protocol error: {s}"),
            Self::Disconnected => f.write_str("CAN source disconnected"),
        }
    }
}
impl std::error::Error for ReceiveError {}

/// A polled, receive-only frame stream. A successful None means no frame was
/// available within the timeout. There is intentionally no send method.
pub trait CanReceive {
    fn descriptor(&self) -> &ReceiveDescriptor;
    fn receive(&mut self, timeout: Duration) -> Result<Option<CanFrame>, ReceiveError>;
}

/// Deterministic offline/mock adapter using the same receive contract as live devices.
#[derive(Debug, Clone)]
pub struct ReplayReceive {
    descriptor: ReceiveDescriptor,
    frames: VecDeque<CanFrame>,
}
impl ReplayReceive {
    pub fn new(frames: Vec<CanFrame>) -> Self {
        Self {
            descriptor: ReceiveDescriptor {
                name: "Offline replay".into(),
                interface: "capture".into(),
                capabilities: ReceiveCapabilities {
                    classic_can: true, can_fd: true, rtr: true, hardware_timestamps: false,
                },
            },
            frames: frames.into(),
        }
    }
    pub fn remaining(&self) -> usize { self.frames.len() }
}
impl CanReceive for ReplayReceive {
    fn descriptor(&self) -> &ReceiveDescriptor { &self.descriptor }
    fn receive(&mut self, _timeout: Duration) -> Result<Option<CanFrame>, ReceiveError> {
        Ok(self.frames.pop_front())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CanId, FdFlags, FrameKind};

    #[test]
    fn replay_adapter_preserves_frames_and_reports_exhaustion() {
        let frame = CanFrame::new(Some(42), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![1, 2], false, FdFlags::default()).unwrap();
        let mut adapter = ReplayReceive::new(vec![frame.clone()]);
        assert!(adapter.descriptor().capabilities.can_fd);
        assert_eq!(adapter.remaining(), 1);
        assert_eq!(adapter.receive(Duration::ZERO).unwrap(), Some(frame));
        assert_eq!(adapter.receive(Duration::from_millis(10)).unwrap(), None);
    }
}
