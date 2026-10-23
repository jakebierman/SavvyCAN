//! Streaming, receive-only GVRET binary frame parser.
//! Wire layout follows collin80/M2RET CommProtocol.txt and legacy
//! SavvyCAN connections/gvretserial.cpp. Does not send any handshake,
//! bus configuration, flow-control or CAN frames.
use savvycan_core::{CanFrame, CanId, Direction, FdFlags, FrameKind};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GvretDecodeError {
    InvalidFrame(String),
}
impl fmt::Display for GvretDecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self { Self::InvalidFrame(s) => write!(f, "invalid GVRET frame: {s}") }
    }
}
impl std::error::Error for GvretDecodeError {}

#[derive(Debug)]
enum State {
    Idle,
    Command,
    Header { fd: bool, header: Vec<u8> },
    Payload { fd: bool, header: Vec<u8>, data: Vec<u8>, len: usize },
    Skip { remaining: usize },
}

/// Accepts arbitrary byte chunks and yields complete CAN frames. The decoder
/// deliberately ignores metadata replies. A caller must arrange for a GVRET
/// adapter to already emit binary frames; this API never configures devices.
///
/// Classic framing: F1 00 + u32le timestamp + u32le ID/IDE + bus/DLC + data.
/// CAN FD framing: F1 14 + same timestamp/ID + FD length + bus + data.
/// Source firmware may append an optional zero terminator, ignored in idle.
/// Device ticks are relative microseconds and unwrap after u32 rollover.
#[derive(Debug)]
pub struct GvretStreamDecoder {
    state: State,
    last_timestamp: Option<u32>,
    timestamp_epoch: u64,
}
impl Default for GvretStreamDecoder {
    fn default() -> Self { Self::new() }
}
impl GvretStreamDecoder {
    pub fn new() -> Self {
        Self { state: State::Idle, last_timestamp: None, timestamp_epoch: 0 }
    }

    pub fn reset(&mut self) {
        self.state = State::Idle;
        self.last_timestamp = None;
        self.timestamp_epoch = 0;
    }

    fn unwrapped_time(&mut self, timestamp: u32) -> u64 {
        if self.last_timestamp.is_some_and(|last|
            timestamp < last && last - timestamp > u32::MAX / 2) {
            self.timestamp_epoch += 1_u64 << 32;
        }
        self.last_timestamp = Some(timestamp);
        self.timestamp_epoch + u64::from(timestamp)
    }

    fn finish(&mut self, fd: bool, header: &[u8], data: Vec<u8>)
        -> Result<CanFrame, GvretDecodeError>
    {
        let timestamp = u32::from_le_bytes(header[..4].try_into().expect("checked header length"));
        let raw_id = u32::from_le_bytes(header[4..8].try_into().expect("checked header length"));
        let extended = raw_id & 0x8000_0000 != 0;
        let raw_id = raw_id & !0x8000_0000;
        let id = if extended {
            CanId::extended(raw_id)
                .map_err(|e| GvretDecodeError::InvalidFrame(e.to_string()))?
        } else {
            let value = u16::try_from(raw_id).map_err(|_| GvretDecodeError::InvalidFrame(
                format!("standard CAN ID {raw_id:X} exceeds 7FF")
            ))?;
            CanId::standard(value)
                .map_err(|e| GvretDecodeError::InvalidFrame(e.to_string()))?
        };
        let bus = if fd { header[9] } else { header[8] >> 4 };
        let mut frame = CanFrame::new(
            Some(self.unwrapped_time(timestamp)), format!("bus{bus}"),
            id, FrameKind::Data, data, fd, FdFlags::default()
        ).map_err(|e| GvretDecodeError::InvalidFrame(e.to_string()))?;
        frame.direction = Direction::Received;
        Ok(frame)
    }

    /// Returns one decoded frame or error when the supplied byte completes it.
    /// Unrecognized commands are ignored until the next F1 marker.
    pub fn push(&mut self, byte: u8) -> Option<Result<CanFrame, GvretDecodeError>> {
        match std::mem::replace(&mut self.state, State::Idle) {
            State::Idle => {
                if byte == 0xF1 { self.state = State::Command; }
                None
            }
            State::Command => {
                self.state = match byte {
                    0 => State::Header { fd: false, header: Vec::with_capacity(9) },
                    20 => State::Header { fd: true, header: Vec::with_capacity(10) },
                    // Fixed-length metadata replies in legacy GVRET; skip
                    // them so an F1 byte within metadata is not a new frame.
                    1 => State::Skip { remaining: 4 },
                    2 => State::Skip { remaining: 2 },
                    3 => State::Skip { remaining: 9 },
                    6 => State::Skip { remaining: 10 },
                    7 => State::Skip { remaining: 6 },
                    9 => State::Skip { remaining: 2 },
                    12 => State::Skip { remaining: 1 },
                    13 => State::Skip { remaining: 15 },
                    _ => State::Idle,
                };
                None
            }
            State::Header { fd, mut header } => {
                header.push(byte);
                let needed = if fd { 10 } else { 9 };
                if header.len() < needed {
                    self.state = State::Header { fd, header };
                    return None;
                }
                let length_byte = header[8];
                let len = if fd { usize::from(length_byte & 0x3F) }
                    else { usize::from(length_byte & 0x0F) };
                let invalid = if fd {
                    (length_byte & 0xC0 != 0).then_some(
                        "unverified CAN FD length/flag bits (64-byte encoding unsupported)")
                } else if len > 8 {
                    Some("classic CAN DLC exceeds eight bytes")
                } else { None };
                if let Some(error) = invalid {
                    self.state = State::Skip { remaining: len };
                    return Some(Err(GvretDecodeError::InvalidFrame(error.into())));
                }
                if len == 0 { return Some(self.finish(fd, &header, Vec::new())); }
                self.state = State::Payload {
                    fd, header, data: Vec::with_capacity(len), len
                };
                None
            }
            State::Payload { fd, header, mut data, len } => {
                data.push(byte);
                if data.len() < len {
                    self.state = State::Payload { fd, header, data, len };
                    None
                } else {
                    Some(self.finish(fd, &header, data))
                }
            }
            State::Skip { remaining } => {
                if remaining > 1 { self.state = State::Skip { remaining: remaining - 1 }; }
                None
            }
        }
    }

    pub fn push_bytes(&mut self, bytes: &[u8])
        -> Vec<Result<CanFrame, GvretDecodeError>>
    {
        bytes.iter().filter_map(|b| self.push(*b)).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(fd: bool, timestamp: u32, id: u32, bus: u8, data: &[u8]) -> Vec<u8> {
        let mut p = vec![0xF1, if fd { 20 } else { 0 }];
        p.extend_from_slice(&timestamp.to_le_bytes());
        p.extend_from_slice(&id.to_le_bytes());
        if fd { p.push(data.len() as u8); p.push(bus); }
        else { p.push((bus << 4) | data.len() as u8); }
        p.extend_from_slice(data);
        p
    }

    #[test]
    fn chunk_boundaries_embedded_markers_and_multiple_buses() {
        let mut decoder = GvretStreamDecoder::new();
        let mut bytes = vec![0, b'A', 0xE7];
        bytes.extend(packet(false, 100, 0x123, 1, &[0xF1, 0x00, 3]));
        bytes.push(0); // optional terminator
        bytes.extend(packet(false, 101, 0x8000_0123, 3, &[]));
        let mut frames = Vec::new();
        for chunk in bytes.chunks(3) { frames.extend(decoder.push_bytes(chunk)); }
        assert_eq!(frames.len(), 2);
        let first = frames[0].as_ref().unwrap();
        assert_eq!(first.id, CanId::Standard(0x123));
        assert_eq!(first.interface, "bus1");
        assert_eq!(first.data, vec![0xF1, 0, 3]);
        assert_eq!(first.direction, Direction::Received);
        let next = frames[1].as_ref().unwrap();
        assert_eq!(next.id, CanId::Extended(0x123));
        assert_eq!(next.interface, "bus3");
        assert!(next.data.is_empty());
    }

    #[test]
    fn accepts_fd_payload_without_fabricating_brs_flags() {
        let mut d = GvretStreamDecoder::new();
        let data = vec![0x42; 32];
        let frames = d.push_bytes(&packet(true, 20, 0x8000_0123, 2, &data));
        assert_eq!(frames.len(), 1);
        let f = frames[0].as_ref().unwrap();
        assert!(f.is_fd);
        assert_eq!(f.data, data);
        assert!(!f.fd_flags.bitrate_switch);
        assert!(!f.fd_flags.error_state_indicator);
        assert_eq!(f.interface, "bus2");
    }

    #[test]
    fn unwraps_microsecond_counter_and_skips_metadata() {
        let mut d = GvretStreamDecoder::new();
        let mut bytes = packet(false, u32::MAX - 3, 0x123, 0, &[1]);
        bytes.extend([0xF1, 1, 0xF1, 0x00, 0xFF, 0x00]);
        bytes.extend(packet(false, 7, 0x123, 0, &[2]));
        let frames = d.push_bytes(&bytes);
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].as_ref().unwrap().timestamp_us, Some(u64::from(u32::MAX - 3)));
        assert_eq!(frames[1].as_ref().unwrap().timestamp_us, Some((1_u64 << 32) + 7));
    }

    #[test]
    fn malformed_identifiers_lengths_and_recovery() {
        let mut d = GvretStreamDecoder::new();
        let mut wrong_dlc = packet(false, 1, 0x123, 0, &[1; 9]);
        let invalid = d.push_bytes(&wrong_dlc);
        assert!(matches!(invalid[0], Err(GvretDecodeError::InvalidFrame(_))));
        wrong_dlc.clear();
        wrong_dlc.extend(packet(false, 2, 0x8000_0000 | 0x2000_0000, 0, &[1]));
        wrong_dlc.extend(packet(false, 3, 0x123, 0, &[2]));
        let results = d.push_bytes(&wrong_dlc);
        assert!(results[0].is_err());
        assert_eq!(results[1].as_ref().unwrap().data, vec![2]);
    }

    #[test]
    fn reset_discards_partial_packets() {
        let mut d = GvretStreamDecoder::new();
        d.push_bytes(&packet(false, 100, 0x123, 0, &[1, 2])[..6]);
        d.reset();
        let frames = d.push_bytes(&packet(false, 200, 0x456, 0, &[3]));
        assert_eq!(frames[0].as_ref().unwrap().id, CanId::Standard(0x456));
    }
}
