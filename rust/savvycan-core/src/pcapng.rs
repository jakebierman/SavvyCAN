//! Validated PCAPNG reader for LINKTYPE_CAN_SOCKETCAN (227).
//! Handles sections, per-interface timestamp resolutions and Enhanced
//! Packet Blocks. Unsupported linktypes are rejected rather than decoded
//! under a guessed packet layout.
use crate::file_formats::{ExportError, ImportError, ImportedTrace, TraceFormat};
use crate::{Direction, CanFrame, FrameKind};
use crate::pcap::parse_frame;
use std::collections::HashMap;
use std::io::{Read, Write};

const SECTION_MAGIC: [u8; 4] = [0x0a, 0x0d, 0x0d, 0x0a];
const MAX_BLOCK: usize = 16 * 1024 * 1024;

#[derive(Debug)]
struct CaptureInterface {
    name: String,
    units_per_second: u128,
    offset_seconds: i64,
    snaplen: u32,
}

fn read_u16(bytes: &[u8], little: bool) -> u16 {
    let raw: [u8; 2] = bytes.try_into().expect("two-byte field");
    if little { u16::from_le_bytes(raw) } else { u16::from_be_bytes(raw) }
}
fn read_u32(bytes: &[u8], little: bool) -> u32 {
    let raw: [u8; 4] = bytes.try_into().expect("four-byte field");
    if little { u32::from_le_bytes(raw) } else { u32::from_be_bytes(raw) }
}
fn read_i64(bytes: &[u8], little: bool) -> i64 {
    let raw: [u8; 8] = bytes.try_into().expect("eight-byte field");
    if little { i64::from_le_bytes(raw) } else { i64::from_be_bytes(raw) }
}
fn failure(reason: &str) -> ImportError { ImportError::Unsupported(reason.into()) }

fn options<F>(bytes: &[u8], little: bool, mut consume: F) -> Result<(), ImportError>
where F: FnMut(u16, &[u8]) -> Result<(), ImportError> {
    let mut at = 0usize;
    while at < bytes.len() {
        if bytes.len() - at < 4 { return Err(failure("truncated PCAPNG option header")); }
        let kind = read_u16(&bytes[at..at + 2], little);
        let len = usize::from(read_u16(&bytes[at + 2..at + 4], little));
        at += 4;
        if kind == 0 {
            if len != 0 { return Err(failure("nonzero PCAPNG option terminator length")); }
            return Ok(());
        }
        let padded = (len + 3) & !3;
        if at.checked_add(padded).is_none_or(|end| end > bytes.len()) {
            return Err(failure("truncated PCAPNG option value"));
        }
        consume(kind, &bytes[at..at + len])?;
        at += padded;
    }
    Ok(())
}

fn block_payload(body: &[u8], expected_len: usize, little: bool)
    -> Result<&[u8], ImportError>
{
    if body.len() < 4 || read_u32(&body[body.len() - 4..], little) as usize != expected_len {
        return Err(failure("PCAPNG block's repeated length does not match"));
    }
    Ok(&body[..body.len() - 4])
}

fn read_section<R: Read>(
    reader: &mut R, length_bytes: [u8;4],
) -> Result<bool, ImportError> {
    // The byte-order magic follows the 8-byte block header and determines
    // how its own length and the entire section are encoded.
    let mut bom = [0u8; 4];
    reader.read_exact(&mut bom)?;
    let little = match bom {
        [0x4d, 0x3c, 0x2b, 0x1a] => true,
        [0x1a, 0x2b, 0x3c, 0x4d] => false,
        _ => return Err(failure("invalid PCAPNG byte-order magic")),
    };
    let length = read_u32(&length_bytes, little) as usize;
    if !(28..=MAX_BLOCK).contains(&length) || length % 4 != 0 {
        return Err(failure("invalid PCAPNG section length"));
    }
    let mut rest = vec![0u8; length - 12];
    reader.read_exact(&mut rest)?;
    let content = block_payload(&rest, length, little)?;
    if content.len() < 12 || read_u16(&content[..2], little) != 1 ||
        read_u16(&content[2..4], little) != 0 {
        return Err(failure("PCAPNG section requires version 1.0"));
    }
    Ok(little)
}

/// Each block is bounded to avoid allocating untrusted capture-sized metadata.
pub fn read_socketcan_pcapng<R: Read>(mut reader: R)
    -> Result<ImportedTrace, ImportError>
{
    let mut little: Option<bool> = None;
    let mut interfaces: Vec<CaptureInterface> = Vec::new();
    let mut frames: Vec<CanFrame> = Vec::new();
    let mut section = 0usize;
    let mut block_index = 0usize;
    loop {
        let mut header = [0u8; 8];
        if reader.read(&mut header[..1])? == 0 { break; }
        reader.read_exact(&mut header[1..])?;
        block_index += 1;
        if header[..4] == SECTION_MAGIC {
            let next_little = read_section(&mut reader, header[4..8].try_into().unwrap())?;
            if little.is_some() { section += 1; }
            little = Some(next_little);
            interfaces.clear();
            continue;
        }
        let endian = little.ok_or_else(|| failure("PCAPNG must begin with a section header"))?;
        let kind = read_u32(&header[..4], endian);
        let size = read_u32(&header[4..8], endian) as usize;
        if !(12..=MAX_BLOCK).contains(&size) || size % 4 != 0 {
            return Err(failure("invalid PCAPNG block size"));
        }
        let mut rest = vec![0u8; size - 8];
        reader.read_exact(&mut rest)?;
        let body = block_payload(&rest, size, endian)?;
        match kind {
            1 => {
                if body.len() < 8 { return Err(failure("truncated PCAPNG interface block")); }
                let linktype = read_u16(&body[..2], endian);
                if linktype != 227 {
                    return Err(failure(&format!(
                        "PCAPNG linktype {linktype} is not LINKTYPE_CAN_SOCKETCAN (227)"
                    )));
                }
                let snaplen = read_u32(&body[4..8], endian);
                if snaplen < 16 { return Err(failure("PCAPNG CAN snaplen shorter than 16 bytes")); }
                let index = interfaces.len();
                let mut iface = CaptureInterface {
                    name: if section == 0 { format!("bus{index}") }
                        else { format!("section{section}_bus{index}") },
                    units_per_second: 1_000_000,
                    offset_seconds: 0,
                    snaplen,
                };
                options(&body[8..], endian, |code, value| {
                    match code {
                        2 => {
                            if value.len() > 256 {
                                return Err(failure("PCAPNG interface name exceeds 256 bytes"));
                            }
                            let text = std::str::from_utf8(value)
                                .map_err(|_| failure("PCAPNG interface name is not UTF-8"))?;
                            if !text.is_empty() { iface.name = text.to_owned(); }
                        }
                        9 => {
                            if value.len() != 1 {
                                return Err(failure("PCAPNG if_tsresol must be one byte"));
                            }
                            let magnitude = (value[0] & 0x7f) as u32;
                            let base: u128 = if value[0] & 0x80 != 0 { 2 } else { 10 };
                            iface.units_per_second = base.checked_pow(magnitude)
                                .filter(|units| *units != 0)
                                .ok_or_else(|| failure("PCAPNG timestamp resolution overflows"))?;
                        }
                        14 => {
                            if value.len() != 8 {
                                return Err(failure("PCAPNG if_tsoffset must be eight bytes"));
                            }
                            iface.offset_seconds = read_i64(value, endian);
                        }
                        _ => {}
                    }
                    Ok(())
                })?;
                interfaces.push(iface);
            }
            6 => {
                if body.len() < 20 { return Err(failure("truncated PCAPNG enhanced packet")); }
                let index = read_u32(&body[..4], endian) as usize;
                let iface = interfaces.get(index)
                    .ok_or_else(|| failure("PCAPNG packet references unknown interface"))?;
                let ticks = (u64::from(read_u32(&body[4..8], endian)) << 32)
                    | u64::from(read_u32(&body[8..12], endian));
                let captured = read_u32(&body[12..16], endian) as usize;
                let original = read_u32(&body[16..20], endian) as usize;
                if captured != original || (captured != 16 && captured != 72) ||
                    captured > iface.snaplen as usize {
                    return Err(failure("truncated or unsupported PCAPNG SocketCAN packet"));
                }
                let padded = (captured + 3) & !3;
                if body.len() < 20 + padded {
                    return Err(failure("PCAPNG packet data is truncated"));
                }
                let micros = (u128::from(ticks) * 1_000_000u128) / iface.units_per_second;
                let stamped = i128::try_from(micros)
                    .ok().and_then(|n| n.checked_add(i128::from(iface.offset_seconds) * 1_000_000))
                    .and_then(|n| u64::try_from(n).ok())
                    .ok_or_else(|| failure("PCAPNG timestamp is out of microsecond range"))?;
                let mut packet = parse_frame(
                    &body[20..20 + captured], Some(stamped), frames.len() + 1
                )?;
                packet.interface = iface.name.clone();
                options(&body[20 + padded..], endian, |code, value| {
                    if code == 2 {
                        if value.len() != 4 {
                            return Err(failure("PCAPNG epb_flags must be four bytes"));
                        }
                        let flags = read_u32(value, endian);
                        packet.direction = match flags & 3 {
                            1 => Direction::Received,
                            2 => Direction::Transmitted,
                            _ => Direction::Unknown,
                        };
                    }
                    Ok(())
                })?;
                frames.push(packet);
            }
            // Metadata blocks and obsolete packet kinds aren't CAN frames.
            // Simple Packet Blocks have no timestamp and are not supported yet.
            3 => return Err(failure("PCAPNG Simple Packet Blocks are not yet supported")),
            _ => {}
        }
    }
    if little.is_none() { return Err(ImportError::Empty); }
    let _ = block_index; // reserved for future precise block-level diagnostics
    Ok(ImportedTrace { format: TraceFormat::SocketCanPcapNg, frames })
}


/// Export PCAPNG with one Interface Description Block per source interface.
/// Unlike classic PCAP, both bus and Rx/Tx direction can be preserved.
/// Timestamps must be known; no placeholder values are fabricated.
pub fn write_socketcan_pcapng<W: Write>(
    mut writer: W, frames: &[CanFrame],
) -> Result<(), ExportError> {
    let mut interface_index: HashMap<&str, u32> = HashMap::new();
    let mut interfaces: Vec<&str> = Vec::new();
    for (index, frame) in frames.iter().enumerate() {
        if frame.timestamp_us.is_none() {
            return Err(ExportError::Unsupported {
                index, reason: "SocketCAN PCAPNG export requires frame timestamps".into(),
            });
        }
        if frame.interface.len() > u16::MAX as usize {
            return Err(ExportError::Unsupported {
                index, reason: "PCAPNG interface name is too long".into(),
            });
        }
        if !interface_index.contains_key(frame.interface.as_str()) {
            let next = u32::try_from(interfaces.len()).map_err(|_| ExportError::Unsupported {
                index, reason: "too many PCAPNG interfaces".into(),
            })?;
            interface_index.insert(frame.interface.as_str(), next);
            interfaces.push(frame.interface.as_str());
        }
    }

    fn option(out: &mut Vec<u8>, code: u16, value: &[u8]) {
        out.extend_from_slice(&code.to_le_bytes());
        out.extend_from_slice(&(value.len() as u16).to_le_bytes());
        out.extend_from_slice(value);
        while out.len() % 4 != 0 { out.push(0); }
    }
    fn block<W: Write>(
        writer: &mut W, block_type: u32, content: &[u8],
    ) -> Result<(), std::io::Error> {
        let length = (content.len() + 12) as u32;
        writer.write_all(&block_type.to_le_bytes())?;
        writer.write_all(&length.to_le_bytes())?;
        writer.write_all(content)?;
        writer.write_all(&length.to_le_bytes())
    }

    let mut header = Vec::new();
    header.extend_from_slice(&0x1a2b3c4d_u32.to_le_bytes());
    header.extend_from_slice(&1_u16.to_le_bytes());
    header.extend_from_slice(&0_u16.to_le_bytes());
    header.extend_from_slice(&(-1_i64).to_le_bytes());
    block(&mut writer, 0x0a0d0d0a, &header)?;

    for &name in &interfaces {
        let mut body = Vec::new();
        body.extend_from_slice(&227_u16.to_le_bytes());
        body.extend_from_slice(&0_u16.to_le_bytes());
        body.extend_from_slice(&72_u32.to_le_bytes());
        option(&mut body, 2, name.as_bytes());
        option(&mut body, 9, &[6]); // microseconds (10^-6)
        option(&mut body, 0, &[]);
        block(&mut writer, 1, &body)?;
    }

    for frame in frames {
        let index = interface_index[frame.interface.as_str()];
        let timestamp = frame.timestamp_us.expect("prevalidated");
        let length = if frame.is_fd { 72_u32 } else { 16_u32 };
        let mut body = Vec::with_capacity(20 + length as usize + 16);
        body.extend_from_slice(&index.to_le_bytes());
        body.extend_from_slice(&((timestamp >> 32) as u32).to_le_bytes());
        body.extend_from_slice(&(timestamp as u32).to_le_bytes());
        body.extend_from_slice(&length.to_le_bytes());
        body.extend_from_slice(&length.to_le_bytes());
        let mut wire = [0_u8; 72];
        let mut id = frame.id.value();
        if frame.id.is_extended() { id |= 0x8000_0000; }
        if matches!(frame.kind, FrameKind::RemoteRequest { .. }) { id |= 0x4000_0000; }
        wire[..4].copy_from_slice(&id.to_be_bytes());
        wire[4] = frame.len() as u8;
        if frame.is_fd {
            wire[5] = u8::from(frame.fd_flags.bitrate_switch)
                | (u8::from(frame.fd_flags.error_state_indicator) << 1);
        }
        wire[8..8 + frame.data.len()].copy_from_slice(&frame.data);
        body.extend_from_slice(&wire[..length as usize]);
        let direction = match frame.direction {
            Direction::Received => 1_u32,
            Direction::Transmitted => 2_u32,
            Direction::Unknown => 0_u32,
        };
        if direction != 0 { option(&mut body, 2, &direction.to_le_bytes()); }
        option(&mut body, 0, &[]);
        block(&mut writer, 6, &body)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CanId, FrameKind};
    use std::io::Cursor;

    fn short(value: u16, little: bool) -> [u8;2] {
        if little { value.to_le_bytes() } else { value.to_be_bytes() }
    }
    fn word(value: u32, little: bool) -> [u8;4] {
        if little { value.to_le_bytes() } else { value.to_be_bytes() }
    }
    fn block(out: &mut Vec<u8>, kind: u32, body: &[u8], little: bool) {
        let len = (body.len() + 12) as u32;
        out.extend(word(kind, little));
        out.extend(word(len, little));
        out.extend(body);
        out.extend(word(len, little));
    }
    fn option(out: &mut Vec<u8>, kind: u16, value: &[u8], little: bool) {
        out.extend(short(kind, little));
        out.extend(short(value.len() as u16, little));
        out.extend(value);
        while out.len() % 4 != 0 { out.push(0); }
    }
    fn pcapng(little: bool) -> Vec<u8> {
        let mut bytes = Vec::new();
        let mut shb = Vec::new();
        shb.extend(word(0x1a2b3c4d, little));
        shb.extend(short(1, little));
        shb.extend(short(0, little));
        shb.extend(if little { (-1_i64).to_le_bytes() } else { (-1_i64).to_be_bytes() });
        block(&mut bytes, 0x0a0d0d0a, &shb, little);
        for (name, resolution) in [("can0", 6_u8), ("can1", 0x80|10_u8)] {
            let mut idb = Vec::new();
            idb.extend(short(227, little));
            idb.extend(short(0, little));
            idb.extend(word(72, little));
            option(&mut idb, 2, name.as_bytes(), little);
            option(&mut idb, 9, &[resolution], little);
            option(&mut idb, 0, &[], little);
            block(&mut bytes, 1, &idb, little);
        }
        for (interface, id, flags, ticks, direction) in [
            (0_u32, 0x123_u32, 0_u8, 1_500_000_u64, 1_u32),
            (1, 0x8000_0123, 3, 1536, 2),
        ] {
            let mut epb = Vec::new();
            epb.extend(word(interface, little));
            epb.extend(word((ticks >> 32) as u32, little));
            epb.extend(word(ticks as u32, little));
            epb.extend(word(16, little));
            epb.extend(word(16, little));
            epb.extend(id.to_be_bytes());
            epb.push(2);
            epb.push(flags);
            epb.extend([0, 0]);
            epb.extend([0xaa, 0xbb, 0, 0, 0, 0, 0, 0]);
            option(&mut epb, 2, &word(direction, little), little);
            option(&mut epb, 0, &[], little);
            block(&mut bytes, 6, &epb, little);
        }
        bytes
    }

    #[test]
    fn decodes_two_interfaces_both_endian_and_timestamp_resolutions() {
        for little in [true, false] {
            let trace = read_socketcan_pcapng(Cursor::new(pcapng(little))).unwrap();
            assert_eq!(trace.format, TraceFormat::SocketCanPcapNg);
            assert_eq!(trace.frames.len(), 2);
            assert_eq!(trace.frames[0].interface, "can0");
            assert_eq!(trace.frames[0].timestamp_us, Some(1_500_000));
            assert_eq!(trace.frames[0].id, CanId::Standard(0x123));
            assert_eq!(trace.frames[0].direction, Direction::Received);
            assert_eq!(trace.frames[1].interface, "can1");
            assert_eq!(trace.frames[1].timestamp_us, Some(1_500_000));
            assert_eq!(trace.frames[1].id, CanId::Extended(0x123));
            assert_eq!(trace.frames[1].direction, Direction::Transmitted);
            assert_eq!(trace.frames[1].kind, FrameKind::Data);
        }
    }


    #[test]
    fn pcapng_writer_round_trips_multiple_buses_direction_fd_and_rtr() {
        use crate::{CanId, FdFlags};
        let mut classic = CanFrame::new(
            Some(123_456_000), "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![0xaa, 0xbb], false, FdFlags::default()
        ).unwrap();
        classic.direction = Direction::Received;
        let mut fd = CanFrame::new(
            Some(123_456_007), "can1", CanId::Extended(0x18ff50e5),
            FrameKind::Data, vec![0x11; 64], true,
            FdFlags { bitrate_switch: true, error_state_indicator: true }
        ).unwrap();
        fd.direction = Direction::Transmitted;
        let rtr = CanFrame::new(
            Some(123_456_012), "can0", CanId::Standard(0x456),
            FrameKind::RemoteRequest { dlc: 8 }, vec![], false, FdFlags::default()
        ).unwrap();
        let original = [classic, fd, rtr];
        let mut output = Vec::new();
        write_socketcan_pcapng(&mut output, &original).unwrap();
        let parsed = read_socketcan_pcapng(Cursor::new(output)).unwrap();
        assert_eq!(parsed.frames, original);
    }

    #[test]
    fn pcapng_writer_rejects_unknown_time_before_io() {
        use crate::{CanId, FdFlags};
        let frame = CanFrame::new(
            None, "can0", CanId::Standard(0x123),
            FrameKind::Data, vec![], false, FdFlags::default()
        ).unwrap();
        let mut output = Vec::new();
        assert!(write_socketcan_pcapng(&mut output, &[frame]).is_err());
        assert!(output.is_empty());
    }

    #[test]
    fn validates_packet_lengths_blocks_and_interface_ids() {
        let mut file = pcapng(true);
        let end = file.len();
        file[end - 4..].copy_from_slice(&0_u32.to_le_bytes());
        assert!(read_socketcan_pcapng(Cursor::new(file)).is_err());
        let mut file = pcapng(true);
        file.truncate(file.len() - 2);
        assert!(read_socketcan_pcapng(Cursor::new(file)).is_err());
        let mut file = pcapng(true);
        // First EPB starts after section (28) + two IDBs (variable).
        let epb = file.windows(4).position(|w| w == [6, 0, 0, 0]).unwrap();
        file[epb + 8..epb + 12].copy_from_slice(&99_u32.to_le_bytes());
        assert!(read_socketcan_pcapng(Cursor::new(file)).is_err());
    }
}
