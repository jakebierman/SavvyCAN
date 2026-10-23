//! Classic PCAP LINKTYPE_CAN_SOCKETCAN (227) import. No libpcap runtime needed.
//! pcapng and Linux cooked captures are separate formats, rejected explicitly.
use crate::{CanFrame, CanId, Direction, FdFlags, FrameKind};
use crate::file_formats::{ExportError, ImportError, ImportedTrace, TraceFormat};
use std::io::{Read, Write};

pub fn is_pcap_magic(buffer: &[u8]) -> bool {
    if buffer.len() < 4 { return false; }
    matches!(&buffer[..4],
        [0xd4, 0xc3, 0xb2, 0xa1] |
        [0xa1, 0xb2, 0xc3, 0xd4] |
        [0x4d, 0x3c, 0xb2, 0xa1] |
        [0xa1, 0xb2, 0x3c, 0x4d])
}

fn number(raw: &[u8], little: bool) -> u32 {
    let bytes: [u8; 4] = raw.try_into().expect("exact four bytes");
    if little { u32::from_le_bytes(bytes) } else { u32::from_be_bytes(bytes) }
}

fn parse_frame(
    packet: &[u8], timestamp_us: u64, number: usize,
) -> Result<CanFrame, ImportError> {
    let bad = |reason: &str| ImportError::Line {
        number, reason: reason.into()
    };
    if packet.len() != 16 && packet.len() != 72 {
        return Err(bad("SocketCAN PCAP packet must be 16 or 72 bytes"));
    }
    // DLT_CAN_SOCKETCAN encodes the CAN-ID/flag word in network byte order,
    // regardless of the PCAP file's byte order.
    let word = u32::from_be_bytes(packet[..4].try_into().expect("checked length"));
    if word & 0x2000_0000 != 0 {
        return Err(bad("CAN controller error frames are not represented by CanFrame"));
    }
    let is_fd = packet.len() == 72;
    if word & 0x8000_0000 == 0 && (word & 0x1fff_ffff) > u32::from(CanId::MAX_STANDARD) {
        return Err(bad("standard SocketCAN PCAP ID exceeds 7FF"));
    }
    let raw_id = if word & 0x8000_0000 != 0 {
        CanId::extended(word & CanId::MAX_EXTENDED)
    } else {
        CanId::standard((word & u32::from(CanId::MAX_STANDARD)) as u16)
    }.map_err(|error| bad(&error.to_string()))?;
    let length = usize::from(packet[4]);
    let fd_flags = if is_fd {
        if packet[5] & !0x07 != 0 {
            return Err(bad("unknown SocketCAN FD frame flags"));
        }
        FdFlags {
            bitrate_switch: packet[5] & 0x01 != 0,
            error_state_indicator: packet[5] & 0x02 != 0,
        }
    } else { FdFlags::default() };
    let kind = if word & 0x4000_0000 != 0 {
        if is_fd { return Err(bad("remote request cannot be a CAN FD frame")); }
        FrameKind::RemoteRequest { dlc: length.try_into().map_err(|_| bad("RTR DLC overflows u8"))? }
    } else { FrameKind::Data };
    let max = if is_fd { 64 } else { 8 };
    if length > max { return Err(bad("SocketCAN PCAP DLC exceeds frame capacity")); }
    let bytes = if matches!(kind, FrameKind::RemoteRequest { .. }) {
        Vec::new()
    } else { packet[8..8 + length].to_vec() };
    let mut frame = CanFrame::new(
        Some(timestamp_us), "bus0", raw_id, kind, bytes, is_fd, fd_flags
    ).map_err(|error| bad(&error.to_string()))?;
    // Standard PCAP does not record Tx/Rx direction for this link type.
    frame.direction = Direction::Unknown;
    Ok(frame)
}

pub fn read_socketcan_pcap<R: Read>(mut reader: R) -> Result<ImportedTrace, ImportError> {
    let mut header = [0u8; 24];
    reader.read_exact(&mut header)?;
    let (little, nano) = match &header[..4] {
        [0xd4, 0xc3, 0xb2, 0xa1] => (true, false),
        [0xa1, 0xb2, 0xc3, 0xd4] => (false, false),
        [0x4d, 0x3c, 0xb2, 0xa1] => (true, true),
        [0xa1, 0xb2, 0x3c, 0x4d] => (false, true),
        _ => return Err(ImportError::Unsupported("unknown PCAP magic".into())),
    };
    let short = |slice: &[u8]| {
        let bytes: [u8; 2] = slice.try_into().expect("two bytes");
        if little { u16::from_le_bytes(bytes) } else { u16::from_be_bytes(bytes) }
    };
    if short(&header[4..6]) != 2 || short(&header[6..8]) != 4 {
        return Err(ImportError::Unsupported("PCAP version must be 2.4".into()));
    }
    let snaplen = number(&header[16..20], little);
    let linktype = number(&header[20..24], little);
    if linktype != 227 {
        return Err(ImportError::Unsupported(
            format!("PCAP linktype {linktype} is not LINKTYPE_CAN_SOCKETCAN (227)")
        ));
    }
    if snaplen < 16 {
        return Err(ImportError::Unsupported("PCAP snaplen is shorter than a CAN frame".into()));
    }
    let mut frames = Vec::new();
    loop {
        let mut packet_header = [0u8; 16];
        if reader.read(&mut packet_header[..1])? == 0 { break; }
        reader.read_exact(&mut packet_header[1..])?;
        let seconds = u64::from(number(&packet_header[..4], little));
        let fraction = u64::from(number(&packet_header[4..8], little));
        let included = number(&packet_header[8..12], little);
        let original = number(&packet_header[12..16], little);
        let packet_number = frames.len() + 1;
        if included != original || (included != 16 && included != 72) ||
            included > snaplen {
            return Err(ImportError::Line {
                number: packet_number,
                reason: "truncated or unsupported SocketCAN PCAP packet length".into(),
            });
        }
        let precision = if nano { 1_000_000_000 } else { 1_000_000 };
        if fraction >= precision {
            return Err(ImportError::Line {
                number: packet_number, reason: "invalid PCAP fractional timestamp".into(),
            });
        }
        let timestamp = seconds.checked_mul(1_000_000)
            .and_then(|n| n.checked_add(if nano { fraction / 1_000 } else { fraction }))
            .ok_or_else(|| ImportError::Line {
                number: packet_number, reason: "PCAP timestamp overflow".into(),
            })?;
        let mut packet = [0u8; 72];
        reader.read_exact(&mut packet[..included as usize])?;
        frames.push(parse_frame(&packet[..included as usize], timestamp, packet_number)?);
    }
    Ok(ImportedTrace { format: TraceFormat::SocketCanPcap, frames })
}


/// Write classic little-endian PCAP containing DLT_CAN_SOCKETCAN frames.
/// PCAP requires timestamps; interface and direction cannot be represented.
/// Multiple interfaces are rejected so packets from independent buses are
/// never merged without explicit user selection.
pub fn write_socketcan_pcap<W: Write>(
    mut writer: W, frames: &[CanFrame],
) -> Result<(), ExportError> {
    let iface = frames.first().map(|frame| frame.interface.as_str());
    for (index, frame) in frames.iter().enumerate() {
        if iface != Some(frame.interface.as_str()) {
            return Err(ExportError::Unsupported {
                index, reason: "SocketCAN PCAP cannot represent multiple interfaces".into(),
            });
        }
        let seconds = frame.timestamp_us.ok_or_else(|| ExportError::Unsupported {
            index, reason: "SocketCAN PCAP requires timestamps".into(),
        })? / 1_000_000;
        if seconds > u64::from(u32::MAX) {
            return Err(ExportError::Unsupported {
                index, reason: "PCAP timestamp seconds exceed u32 range".into(),
            });
        }
    }
    writer.write_all(&[0xd4, 0xc3, 0xb2, 0xa1])?;
    writer.write_all(&2_u16.to_le_bytes())?;
    writer.write_all(&4_u16.to_le_bytes())?;
    writer.write_all(&[0u8; 8])?;
    writer.write_all(&72_u32.to_le_bytes())?;
    writer.write_all(&227_u32.to_le_bytes())?;

    for frame in frames {
        let us = frame.timestamp_us.expect("validated above");
        let sec = (us / 1_000_000) as u32;
        writer.write_all(&sec.to_le_bytes())?;
        writer.write_all(&((us % 1_000_000) as u32).to_le_bytes())?;
        let length = if frame.is_fd { 72_u32 } else { 16_u32 };
        writer.write_all(&length.to_le_bytes())?;
        writer.write_all(&length.to_le_bytes())?;

        let mut wire = [0_u8; 72];
        let mut id = frame.id.value();
        if frame.id.is_extended() { id |= 0x8000_0000; }
        if matches!(frame.kind, FrameKind::RemoteRequest { .. }) {
            id |= 0x4000_0000;
        }
        wire[..4].copy_from_slice(&id.to_be_bytes());
        wire[4] = frame.len() as u8;
        if frame.is_fd {
            wire[5] = u8::from(frame.fd_flags.bitrate_switch)
                | (u8::from(frame.fd_flags.error_state_indicator) << 1);
        }
        wire[8..8 + frame.data.len()].copy_from_slice(&frame.data);
        writer.write_all(&wire[..length as usize])?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::file_formats::read_trace;
    use std::io::Cursor;

    fn pcap(nano: bool) -> Vec<u8> {
        let mut bytes = Vec::new();
        if nano { bytes.extend_from_slice(&[0x4d, 0x3c, 0xb2, 0xa1]); }
        else { bytes.extend_from_slice(&[0xd4, 0xc3, 0xb2, 0xa1]); }
        bytes.extend_from_slice(&2_u16.to_le_bytes());
        bytes.extend_from_slice(&4_u16.to_le_bytes());
        bytes.extend_from_slice(&[0; 8]); // timezone + timestamp accuracy
        bytes.extend_from_slice(&72_u32.to_le_bytes());
        bytes.extend_from_slice(&227_u32.to_le_bytes());
        let mut classic = [0u8; 16];
        classic[..4].copy_from_slice(&0x123_u32.to_be_bytes());
        classic[4] = 2;
        classic[8..10].copy_from_slice(&[0xaa, 0xbb]);
        let mut fd = [0u8; 72];
        fd[..4].copy_from_slice(&(0x8000_0000_u32 | 0x18ff_50e5).to_be_bytes());
        fd[4] = 12;
        fd[5] = 3;
        for (i, byte) in fd[8..20].iter_mut().enumerate() { *byte = i as u8; }
        let mut rtr = [0u8; 16];
        rtr[..4].copy_from_slice(&(0x4000_0000_u32 | 0x456).to_be_bytes());
        rtr[4] = 4;
        for (index, packet) in [classic.as_slice(), fd.as_slice(), rtr.as_slice()].iter().enumerate() {
            bytes.extend_from_slice(&1_u32.to_le_bytes());
            let frac = if nano { 500_000_900u32 } else { 500_000u32 };
            bytes.extend_from_slice(&frac.to_le_bytes());
            bytes.extend_from_slice(&(packet.len() as u32).to_le_bytes());
            bytes.extend_from_slice(&(packet.len() as u32).to_le_bytes());
            bytes.extend_from_slice(packet);
            assert!(index < 3);
        }
        bytes
    }

    #[test]
    fn pcap_writer_round_trips_classic_fd_and_rtr() {
        let standard = CanFrame::new(Some(1_234_567), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![0xaa, 0xbb], false, FdFlags::default()).unwrap();
        let fd = CanFrame::new(Some(1_234_600), "can0", CanId::Extended(0x18ff50e5),
            FrameKind::Data, (0..12).collect(), true,
            FdFlags { bitrate_switch: true, error_state_indicator: true }).unwrap();
        let rtr = CanFrame::new(Some(1_234_700), "can0", CanId::Standard(0x456),
            FrameKind::RemoteRequest { dlc: 4 }, Vec::new(), false, FdFlags::default()).unwrap();
        let mut bytes = Vec::new();
        write_socketcan_pcap(&mut bytes, &[standard.clone(), fd.clone(), rtr.clone()]).unwrap();
        let parsed = read_trace(Cursor::new(bytes)).unwrap();
        for (frame, expected) in parsed.frames.iter().zip([standard, fd, rtr]) {
            assert_eq!(frame.timestamp_us, expected.timestamp_us);
            assert_eq!(frame.id, expected.id);
            assert_eq!(frame.kind, expected.kind);
            assert_eq!(frame.data, expected.data);
            assert_eq!(frame.fd_flags, expected.fd_flags);
            assert_eq!(frame.interface, "bus0");
            assert_eq!(frame.direction, Direction::Unknown);
        }
    }

    #[test]
    fn pcap_writer_rejects_missing_timestamps_and_mixed_interfaces() {
        let a = CanFrame::new(None, "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![0], false, FdFlags::default()).unwrap();
        let mut out = Vec::new();
        assert!(write_socketcan_pcap(&mut out, &[a]).is_err());
        let a = CanFrame::new(Some(0), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![0], false, FdFlags::default()).unwrap();
        let b = CanFrame::new(Some(0), "can1", CanId::Standard(0x456),
            FrameKind::Data, vec![1], false, FdFlags::default()).unwrap();
        assert!(write_socketcan_pcap(&mut out, &[a, b]).is_err());
    }

    #[test]
    fn socketcan_pcap_classic_fd_and_remote_preserve_fields() {
        let parsed = read_trace(Cursor::new(pcap(false))).unwrap();
        assert_eq!(parsed.format, TraceFormat::SocketCanPcap);
        assert_eq!(parsed.frames.len(), 3);
        assert_eq!(parsed.frames[0].timestamp_us, Some(1_500_000));
        assert_eq!(parsed.frames[0].id, CanId::Standard(0x123));
        assert_eq!(parsed.frames[0].data, vec![0xaa, 0xbb]);
        assert_eq!(parsed.frames[1].id, CanId::Extended(0x18ff_50e5));
        assert_eq!(parsed.frames[1].data.len(), 12);
        assert!(parsed.frames[1].fd_flags.bitrate_switch);
        assert!(parsed.frames[1].fd_flags.error_state_indicator);
        assert_eq!(parsed.frames[2].kind, FrameKind::RemoteRequest { dlc: 4 });
        assert_eq!(parsed.frames[2].data.len(), 0);
    }

    #[test]
    fn nanosecond_pcap_and_bad_linktype_are_validated() {
        let parsed = read_trace(Cursor::new(pcap(true))).unwrap();
        assert_eq!(parsed.frames[0].timestamp_us, Some(1_500_000));
        let mut unsupported = pcap(false);
        unsupported[20..24].copy_from_slice(&1_u32.to_le_bytes());
        assert!(read_trace(Cursor::new(unsupported)).unwrap_err().to_string().contains("linktype 1"));
    }

    #[test]
    fn truncated_packet_and_bad_dlc_are_not_silently_accepted() {
        let mut bytes = pcap(false);
        bytes.truncate(bytes.len() - 3);
        assert!(read_trace(Cursor::new(bytes)).is_err());
        let mut bytes = pcap(false);
        bytes[24 + 16 + 4] = 9; // first classic packet DLC
        assert!(read_trace(Cursor::new(bytes)).unwrap_err().to_string().contains("DLC"));
    }
}
