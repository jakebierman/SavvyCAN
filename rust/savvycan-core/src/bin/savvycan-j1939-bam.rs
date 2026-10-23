//! Passive J1939-21 broadcast BAM trace inspector. Never opens a CAN device.
use savvycan_core::{read_trace, CanId, J1939BamAssembler};
use std::error::Error;
use std::fs::File;
use std::io::BufReader;

const HELP: &str = "Usage: savvycan-j1939-bam <trace-file> [source-address-hex] [interface]\n\
  Example: savvycan-j1939-bam capture.log 80 can0\n\
  Reassembles J1939 broadcast TP.CM BAM / TP.DT from offline frames only.\n\
  Does not transmit flow control, issue requests or configure CAN hardware.";

fn parse_source(raw: &str) -> Result<u8, Box<dyn Error>> {
    let raw = raw.trim().strip_prefix("0x").unwrap_or(raw.trim());
    Ok(u8::from_str_radix(raw, 16).map_err(|_| "source address must be one hexadecimal byte")?)
}

fn run() -> Result<(), Box<dyn Error>> {
    let mut args = std::env::args().skip(1);
    let Some(path) = args.next() else { return Err(HELP.into()); };
    if matches!(path.as_str(), "--help" | "-h") {
        println!("{HELP}");
        return Ok(());
    }
    let source = args.next().map(|s| parse_source(&s)).transpose()?;
    let interface = args.next();
    if args.next().is_some() { return Err(HELP.into()); }

    let trace = read_trace(BufReader::new(File::open(&path)?))?;
    let mut assembler = J1939BamAssembler::new();
    let mut payloads = 0usize;
    let mut invalid = 0usize;
    for (index, frame) in trace.frames.iter().enumerate() {
        if interface.as_ref().is_some_and(|s| &frame.interface != s) { continue; }
        if source.is_some_and(|selected| match frame.id {
            CanId::Extended(raw) => (raw & 0xff) as u8 != selected,
            CanId::Standard(_) => true,
        }) { continue; }
        match assembler.push(frame) {
            Ok(Some(packet)) => {
                payloads += 1;
                let timestamp = packet.end_timestamp_us.map(|t| t.to_string())
                    .unwrap_or_else(|| "-".into());
                let preview = packet.payload.iter().take(48)
                    .map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(" ");
                println!("{timestamp} us  {}  src={:02X}  PGN={:05X}  len={}  {}{}",
                    packet.interface, packet.source, packet.pgn, packet.payload.len(),
                    preview, if packet.payload.len() > 48 { " ..." } else { "" });
            }
            Ok(None) => {}
            Err(error) => {
                invalid += 1;
                eprintln!("Trace frame {}: {error}", index + 1);
            }
        }
    }
    eprintln!("Reassembled {payloads} J1939 BAM messages; {invalid} malformed/orphan         records; {} incomplete broadcasts", assembler.pending_count());
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn source_filter_requires_one_hex_byte() {
        assert_eq!(parse_source("80").unwrap(), 0x80);
        assert_eq!(parse_source("0xFF").unwrap(), 255);
        for bad in ["-1", "100", "xyz", ""] {
            assert!(parse_source(bad).is_err());
        }
    }
}
