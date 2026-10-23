//! Receive-only ISO 15765-2 normal-addressing CAN/CAN FD payload reassembly.
//! Does not issue flow-control frames or transmit any diagnostic requests.
use crate::{CanFrame, CanId, Direction, FrameKind};
use std::collections::HashMap;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IsoTpPacket {
    pub interface: String,
    pub id: CanId,
    pub direction: Direction,
    pub start_timestamp_us: Option<u64>,
    pub end_timestamp_us: Option<u64>,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum IsoTpError {
    InvalidPci(String),
    MissingFirstFrame,
    UnexpectedSequence { expected: u8, actual: u8 },
    ConsecutiveTimeout,
    LengthExceedsLimit { declared: usize, limit: usize },
    TooManySessions,
}
impl fmt::Display for IsoTpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidPci(e) => write!(f, "malformed ISO-TP PCI: {e}"),
            Self::MissingFirstFrame => write!(f, "ISO-TP consecutive frame without a first frame"),
            Self::UnexpectedSequence { expected, actual } =>
                write!(f, "ISO-TP sequence mismatch (expected {expected}, got {actual})"),
            Self::ConsecutiveTimeout => write!(f, "ISO-TP consecutive-frame timestamp gap exceeded"),
            Self::LengthExceedsLimit { declared, limit } =>
                write!(f, "ISO-TP payload length {declared} exceeds configured limit {limit}"),
            Self::TooManySessions => write!(f, "ISO-TP pending-session limit exceeded"),
        }
    }
}
impl std::error::Error for IsoTpError {}

type SessionKey = (String, CanId, u8);

fn direction_code(direction: Direction) -> u8 {
    match direction {
        Direction::Received => 0,
        Direction::Transmitted => 1,
        Direction::Unknown => 2,
    }
}
fn key(frame: &CanFrame) -> SessionKey {
    (frame.interface.clone(), frame.id, direction_code(frame.direction))
}

#[derive(Debug)]
struct Pending {
    data: Vec<u8>,
    expected: usize,
    next_sn: u8,
    start: Option<u64>,
    last: Option<u64>,
}

#[derive(Debug)]
pub struct IsoTpAssembler {
    pending: HashMap<SessionKey, Pending>,
    max_payload: usize,
    max_gap_us: u64,
}
impl Default for IsoTpAssembler {
    fn default() -> Self { Self::with_limits(65_535, 1_000_000) }
}
impl IsoTpAssembler {
    pub fn new() -> Self { Self::default() }

    /// Bound memory use per session and reject unreasonable timestamp gaps.
    pub fn with_limits(max_payload: usize, max_gap_us: u64) -> Self {
        Self { pending: HashMap::new(), max_payload, max_gap_us }
    }

    pub fn reset(&mut self) { self.pending.clear(); }

    pub fn pending_count(&self) -> usize { self.pending.len() }

    /// Process one selected ISO-TP CAN ID. The caller decides which IDs are
    /// diagnostic; arbitrary CAN traffic can resemble ISO-TP control bytes.
    /// Flow-control frames are recognized but never sent or synthesized.
    pub fn push(&mut self, frame: &CanFrame) -> Result<Option<IsoTpPacket>, IsoTpError> {
        if !matches!(frame.kind, FrameKind::Data) || frame.data.is_empty() {
            return Ok(None);
        }
        let data = frame.data.as_slice();
        let session = key(frame);
        match data[0] >> 4 {
            0 => {
                self.pending.remove(&session);
                let short_len = usize::from(data[0] & 0x0f);
                let (length, offset) = if short_len == 0 {
                    if !frame.is_fd || data.len() < 2 {
                        return Err(IsoTpError::InvalidPci(
                            "extended single-frame length requires CAN FD and two PCI bytes".into()
                        ));
                    }
                    (usize::from(data[1]), 2)
                } else { (short_len, 1) };
                if length == 0 || length > data.len().saturating_sub(offset) {
                    return Err(IsoTpError::InvalidPci(
                        "single-frame length exceeds available payload bytes".into()
                    ));
                }
                if length > self.max_payload {
                    return Err(IsoTpError::LengthExceedsLimit {
                        declared: length, limit: self.max_payload
                    });
                }
                Ok(Some(IsoTpPacket {
                    interface: frame.interface.clone(),
                    id: frame.id, direction: frame.direction,
                    start_timestamp_us: frame.timestamp_us,
                    end_timestamp_us: frame.timestamp_us,
                    payload: data[offset..offset + length].to_vec(),
                }))
            }
            1 => {
                if data.len() < 2 {
                    return Err(IsoTpError::InvalidPci("first frame needs two PCI bytes".into()));
                }
                let encoded = (usize::from(data[0] & 0x0f) << 8) | usize::from(data[1]);
                let (length, offset) = if encoded == 0 {
                    if data.len() < 6 {
                        return Err(IsoTpError::InvalidPci(
                            "32-bit first-frame length requires six PCI bytes".into()
                        ));
                    }
                    let bytes: [u8; 4] = data[2..6].try_into().expect("length checked");
                    (u32::from_be_bytes(bytes) as usize, 6)
                } else { (encoded, 2) };
                if length == 0 || length <= data.len() - offset {
                    return Err(IsoTpError::InvalidPci(
                        "first frame length must exceed the first fragment".into()
                    ));
                }
                if length > self.max_payload {
                    return Err(IsoTpError::LengthExceedsLimit {
                        declared: length, limit: self.max_payload
                    });
                }
                if !self.pending.contains_key(&session) && self.pending.len() >= 256 {
                    return Err(IsoTpError::TooManySessions);
                }
                self.pending.insert(session, Pending {
                    data: data[offset..].to_vec(),
                    expected: length,
                    next_sn: 1,
                    start: frame.timestamp_us,
                    last: frame.timestamp_us,
                });
                Ok(None)
            }
            2 => {
                let Some(pending) = self.pending.get(&session) else {
                    return Err(IsoTpError::MissingFirstFrame);
                };
                if let (Some(previous), Some(now)) = (pending.last, frame.timestamp_us) {
                    if now.checked_sub(previous).is_none_or(|gap| gap > self.max_gap_us) {
                        self.pending.remove(&session);
                        return Err(IsoTpError::ConsecutiveTimeout);
                    }
                }
                let actual = data[0] & 0x0f;
                if actual != pending.next_sn {
                    let expected = pending.next_sn;
                    self.pending.remove(&session);
                    return Err(IsoTpError::UnexpectedSequence { expected, actual });
                }
                if data.len() < 2 {
                    self.pending.remove(&session);
                    return Err(IsoTpError::InvalidPci("empty consecutive-frame fragment".into()));
                }
                let pending = self.pending.get_mut(&session).expect("already checked");
                let remaining = pending.expected - pending.data.len();
                pending.data.extend_from_slice(&data[1..1 + remaining.min(data.len() - 1)]);
                pending.next_sn = (pending.next_sn + 1) & 0x0f;
                pending.last = frame.timestamp_us;
                if pending.data.len() != pending.expected {
                    return Ok(None);
                }
                let completed = self.pending.remove(&session).expect("completed session exists");
                Ok(Some(IsoTpPacket {
                    interface: frame.interface.clone(), id: frame.id,
                    direction: frame.direction,
                    start_timestamp_us: completed.start,
                    end_timestamp_us: frame.timestamp_us,
                    payload: completed.data,
                }))
            }
            3 => Ok(None), // passive viewer never sends flow control
            other => Err(IsoTpError::InvalidPci(
                format!("unsupported PCI frame type {other:X}")
            )),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::FdFlags;

    fn frame(us: u64, interface: &str, id: u16, bytes: &[u8], fd: bool) -> CanFrame {
        CanFrame::new(Some(us), interface, CanId::Standard(id),
            FrameKind::Data, bytes.to_vec(), fd, FdFlags::default()).unwrap()
    }

    #[test]
    fn classic_single_frame_discards_only_padding() {
        let mut a = IsoTpAssembler::new();
        let f = frame(50, "can0", 0x7e8, &[0x03, 0x62, 0xf1, 0x90, 0, 0, 0, 0], false);
        let packet = a.push(&f).unwrap().unwrap();
        assert_eq!(packet.payload, vec![0x62, 0xf1, 0x90]);
        assert_eq!(packet.start_timestamp_us, Some(50));
        assert_eq!(packet.end_timestamp_us, Some(50));
    }

    #[test]
    fn first_and_consecutive_frames_reassemble_with_padding() {
        let mut a = IsoTpAssembler::new();
        let ff = frame(100, "can0", 0x7e8,
            &[0x10, 0x0a, 0x62, 0xf1, 0x90, 0x41, 0x42, 0x43], false);
        let cf = frame(120, "can0", 0x7e8,
            &[0x21, 0x44, 0x45, 0x46, 0x47, 0, 0, 0], false);
        assert!(a.push(&ff).unwrap().is_none());
        assert_eq!(a.pending_count(), 1);
        let packet = a.push(&cf).unwrap().unwrap();
        assert_eq!(packet.payload, vec![0x62, 0xf1, 0x90, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47]);
        assert_eq!(packet.end_timestamp_us, Some(120));
        assert_eq!(a.pending_count(), 0);
    }

    #[test]
    fn can_fd_extended_single_frame_and_32_bit_first_frame() {
        let mut a = IsoTpAssembler::new();
        let sf = frame(10, "can0", 0x7e8,
            &[0, 10, 0x62, 1, 2, 3, 4, 5, 6, 7, 8, 9], true);
        assert_eq!(a.push(&sf).unwrap().unwrap().payload.len(), 10);
        let ff = frame(100, "can0", 0x7e8, &[0x10, 0, 0, 0, 0, 10, 0x62, 0xf1], false);
        let cf = frame(110, "can0", 0x7e8,
            &[0x21, 0x90, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06], false);
        assert!(a.push(&ff).unwrap().is_none());
        assert!(a.push(&cf).unwrap().is_none());
        let cf2 = frame(120, "can0", 0x7e8, &[0x22, 0x07], false);
        assert_eq!(a.push(&cf2).unwrap().unwrap().payload.len(), 10);
    }

    #[test]
    fn sequence_error_and_timeout_reset_only_affected_session() {
        let mut a = IsoTpAssembler::with_limits(100, 50);
        let ff = frame(100, "can0", 0x7e8, &[0x10, 10, 1, 2, 3, 4, 5, 6], false);
        a.push(&ff).unwrap();
        a.push(&frame(100, "can1", 0x7e8, &[0x10, 10, 1, 2, 3, 4, 5, 6], false)).unwrap();
        let invalid = frame(101, "can0", 0x7e8, &[0x22, 7, 8, 9, 10], false);
        assert_eq!(a.push(&invalid).unwrap_err(),
            IsoTpError::UnexpectedSequence { expected: 1, actual: 2 });
        assert_eq!(a.pending_count(), 1);
        assert!(matches!(a.push(&frame(160, "can1", 0x7e8,
            &[0x21, 7, 8, 9, 10], false)), Err(IsoTpError::ConsecutiveTimeout)));
        assert_eq!(a.pending_count(), 0);
    }

    #[test]
    fn malformed_lengths_and_remote_frames_do_not_allocate_sessions() {
        let mut a = IsoTpAssembler::with_limits(32, 1000);
        assert!(matches!(a.push(&frame(1, "can0", 0x7e8, &[0x10, 0xff, 1, 2, 3, 4, 5, 6], false)),
            Err(IsoTpError::LengthExceedsLimit { .. })));
        assert!(matches!(a.push(&frame(1, "can0", 0x7e8, &[0x08, 1, 2], false)),
            Err(IsoTpError::InvalidPci(_))));
        assert_eq!(a.pending_count(), 0);
    }
}
