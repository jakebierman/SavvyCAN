//! Passive J1939-21 BAM (Broadcast Announce Message) reassembly.
//! Handles TP.CM BAM and TP.DT from captured extended CAN frames only.
//! Does not transmit flow control, process RTS/CTS or alter bus settings.
use crate::{CanFrame, CanId, FrameKind};
use std::collections::HashMap;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct J1939BamPacket {
    pub interface: String,
    pub source: u8,
    pub pgn: u32,
    pub start_timestamp_us: Option<u64>,
    pub end_timestamp_us: Option<u64>,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum J1939BamError {
    InvalidAnnounce(String),
    OrphanData,
    SequenceMismatch { expected: u8, actual: u8 },
    Timeout,
    TooManySessions,
}
impl fmt::Display for J1939BamError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidAnnounce(s) => write!(f, "invalid J1939 BAM announce: {s}"),
            Self::OrphanData => f.write_str("J1939 TP.DT without active BAM"),
            Self::SequenceMismatch { expected, actual } =>
                write!(f, "J1939 TP.DT sequence mismatch: expected {expected}, got {actual}"),
            Self::Timeout => f.write_str("J1939 BAM fragment time gap exceeded"),
            Self::TooManySessions => f.write_str("J1939 BAM active-session limit reached"),
        }
    }
}
impl std::error::Error for J1939BamError {}

type Key = (String, u8);
#[derive(Debug)]
struct Pending {
    expected: usize,
    packets: u8,
    next_sequence: u8,
    pgn: u32,
    data: Vec<u8>,
    start: Option<u64>,
    last: Option<u64>,
}

#[derive(Debug)]
pub struct J1939BamAssembler {
    sessions: HashMap<Key, Pending>,
    max_gap_us: u64,
    max_sessions: usize,
}
impl Default for J1939BamAssembler {
    fn default() -> Self { Self::new() }
}
impl J1939BamAssembler {
    pub fn new() -> Self {
        Self { sessions: HashMap::new(), max_gap_us: 1_000_000, max_sessions: 256 }
    }
    pub fn with_limits(max_gap_us: u64, max_sessions: usize) -> Self {
        Self { sessions: HashMap::new(), max_gap_us, max_sessions }
    }
    pub fn pending_count(&self) -> usize { self.sessions.len() }
    pub fn reset(&mut self) { self.sessions.clear(); }

    /// The caller may feed a mixed CAN trace. Non-BAM PGNs are ignored.
    /// TP.DT is checked only for broadcast destination (FF).
    pub fn push(&mut self, frame: &CanFrame)
        -> Result<Option<J1939BamPacket>, J1939BamError>
    {
        let CanId::Extended(raw) = frame.id else { return Ok(None); };
        if frame.is_fd || !matches!(frame.kind, FrameKind::Data) { return Ok(None); }
        let source = (raw & 0xff) as u8;
        let destination = ((raw >> 8) & 0xff) as u8;
        let pgn = (raw >> 8) & 0x3ff00;
        if pgn != 0xec00 && pgn != 0xeb00 { return Ok(None); }
        if destination != 0xff { return Ok(None); }
        let session = (frame.interface.clone(), source);
        if pgn == 0xec00 {
            if frame.data.first().copied() != Some(0x20) {
                // Explicitly ignore unicast RTS/CTS/ACK/abort controls.
                return Ok(None);
            }
            let data = &frame.data;
            if data.len() != 8 {
                return Err(J1939BamError::InvalidAnnounce(
                    "TP.CM BAM must carry eight bytes".into()
                ));
            }
            let length = usize::from(u16::from_le_bytes([data[1], data[2]]));
            let count = data[3];
            let announced_pgn = u32::from(data[5]) |
                (u32::from(data[6]) << 8) | (u32::from(data[7]) << 16);
            if length <= 8 || length > 1785 || count == 0 ||
                usize::from(count) != length.div_ceil(7) {
                return Err(J1939BamError::InvalidAnnounce(
                    "BAM size must be 9..1785 and packet count must equal ceil(size/7)".into()
                ));
            }
            if data[4] != 0xff || announced_pgn > 0x3ffff ||
                (((announced_pgn >> 8) & 0xff) < 240 && announced_pgn & 0xff != 0) {
                return Err(J1939BamError::InvalidAnnounce(
                    "invalid BAM reserved byte or target PGN".into()
                ));
            }
            if !self.sessions.contains_key(&session) &&
                self.sessions.len() >= self.max_sessions {
                return Err(J1939BamError::TooManySessions);
            }
            self.sessions.insert(session, Pending {
                expected: length, packets: count, next_sequence: 1,
                pgn: announced_pgn, data: Vec::with_capacity(length),
                start: frame.timestamp_us, last: frame.timestamp_us,
            });
            return Ok(None);
        }
        let Some(pending) = self.sessions.get(&session) else {
            return Err(J1939BamError::OrphanData);
        };
        if frame.data.len() != 8 {
            self.sessions.remove(&session);
            return Err(J1939BamError::InvalidAnnounce(
                "TP.DT must contain sequence byte plus seven payload bytes".into()
            ));
        }
        if let (Some(previous), Some(current)) = (pending.last, frame.timestamp_us) {
            if current.checked_sub(previous).is_none_or(|gap| gap > self.max_gap_us) {
                self.sessions.remove(&session);
                return Err(J1939BamError::Timeout);
            }
        }
        let sequence = frame.data[0];
        if sequence != pending.next_sequence {
            let expected = pending.next_sequence;
            self.sessions.remove(&session);
            return Err(J1939BamError::SequenceMismatch {
                expected, actual: sequence,
            });
        }
        let pending = self.sessions.get_mut(&session).expect("checked session");
        let remaining = pending.expected - pending.data.len();
        pending.data.extend_from_slice(&frame.data[1..1 + remaining.min(7)]);
        pending.last = frame.timestamp_us;
        if sequence != pending.packets { pending.next_sequence += 1; return Ok(None); }
        let complete = self.sessions.remove(&session).expect("completed session");
        if complete.data.len() != complete.expected {
            return Err(J1939BamError::InvalidAnnounce(
                "TP.DT finished before announced size was reached".into()
            ));
        }
        Ok(Some(J1939BamPacket {
            interface: frame.interface.clone(),
            source, pgn: complete.pgn,
            start_timestamp_us: complete.start,
            end_timestamp_us: frame.timestamp_us,
            payload: complete.data,
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::FdFlags;

    fn can(id: u32, us: u64, interface: &str, bytes: &[u8]) -> CanFrame {
        CanFrame::new(Some(us), interface, CanId::Extended(id), FrameKind::Data,
            bytes.to_vec(), false, FdFlags::default()).unwrap()
    }
    fn bam(sa: u8, at: u64, iface: &str) -> CanFrame {
        can(0x18ecff00 | u32::from(sa), at, iface,
            &[0x20, 20, 0, 3, 0xff, 0, 0x23, 0x01])
    }
    fn dt(sa: u8, at: u64, iface: &str, seq: u8, data: [u8;7]) -> CanFrame {
        let mut bytes = vec![seq];
        bytes.extend(data);
        can(0x18ebff00 | u32::from(sa), at, iface, &bytes)
    }

    #[test]
    fn reconstructs_linux_can_j1939_bam_fixture_without_padding() {
        let mut a = J1939BamAssembler::new();
        assert!(a.push(&bam(0x80, 100, "can0")).unwrap().is_none());
        assert!(a.push(&dt(0x80, 150, "can0", 1,
            [0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd])).unwrap().is_none());
        assert!(a.push(&dt(0x80, 200, "can0", 2,
            [0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab])).unwrap().is_none());
        let packet = a.push(&dt(0x80, 250, "can0", 3,
            [0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0xff])).unwrap().unwrap();
        assert_eq!(packet.pgn, 0x12300);
        assert_eq!(packet.payload.len(), 20);
        assert_eq!(packet.payload[19], 0x67);
        assert_eq!(packet.source, 0x80);
        assert_eq!(packet.start_timestamp_us, Some(100));
        assert_eq!(packet.end_timestamp_us, Some(250));
        assert_eq!(a.pending_count(), 0);
    }

    #[test]
    fn sessions_are_isolated_by_source_and_interface() {
        let mut a = J1939BamAssembler::new();
        for (source, iface) in [(0x80, "can0"), (0x81, "can0"), (0x80, "can1")] {
            a.push(&bam(source, 100, iface)).unwrap();
        }
        assert_eq!(a.pending_count(), 3);
        assert!(a.push(&dt(0x81, 200, "can0", 1, [1;7])).unwrap().is_none());
        assert_eq!(a.pending_count(), 3);
        assert!(a.push(&dt(0x80, 200, "can0", 2, [1;7])).is_err());
        assert_eq!(a.pending_count(), 2);
    }

    #[test]
    fn rejects_inconsistent_metadata_and_elapsed_time() {
        let mut a = J1939BamAssembler::with_limits(10, 1);
        let bad = can(0x18ecff80, 100, "can0",
            &[0x20, 20, 0, 2, 0xff, 0, 0x23, 0x01]);
        assert!(matches!(a.push(&bad), Err(J1939BamError::InvalidAnnounce(_))));
        a.push(&bam(0x80, 100, "can0")).unwrap();
        assert_eq!(a.push(&dt(0x80, 200, "can0", 1, [1;7])).unwrap_err(),
            J1939BamError::Timeout);
        assert_eq!(a.pending_count(), 0);
        assert_eq!(a.push(&dt(0x80, 201, "can0", 1, [1;7])).unwrap_err(),
            J1939BamError::OrphanData);
    }

    #[test]
    fn ignores_standard_ids_non_tp_fd_and_unicast_control() {
        let mut a = J1939BamAssembler::new();
        let normal = CanFrame::new(Some(1), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![1], false, FdFlags::default()).unwrap();
        assert!(a.push(&normal).unwrap().is_none());
        let control = can(0x18ec9080, 1, "can0",
            &[0x10,20,0,3,3,0,0x23,0x01]);
        assert!(a.push(&control).unwrap().is_none());
        assert_eq!(a.pending_count(), 0);
    }
}
