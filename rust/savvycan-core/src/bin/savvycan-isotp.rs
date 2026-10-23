//! Read-only diagnostic trace inspector. Never opens or transmits to a CAN adapter.
use savvycan_core::{read_trace, IsoTpAssembler, TraceFilter};
use std::error::Error;
use std::fs::File;
use std::io::BufReader;

const HELP: &str = "Usage: savvycan-isotp <trace-file> <CAN-ID-filter> [interface]\n\
  CAN-ID examples: S:7E8, X:18DAF110, PGN:EA00\n\
  Reassembles normal-addressed ISO-TP from imported frames only.\n\
  No CAN transmission, flow-control, or diagnostic request is performed.";

fn describe_payload(bytes: &[u8]) -> String {
    if bytes.len() >= 3 && bytes[0] == 0x7f {
        format!("UDS-like negative response to {:02X}, NRC {:02X}", bytes[1], bytes[2])
    } else if let Some(&sid) = bytes.first() {
        if (0x40..=0x7e).contains(&sid) {
            format!("UDS-like positive response SID {sid:02X}")
        } else {
            format!("payload SID {sid:02X} (not classified)")
        }
    } else { "empty payload".into() }
}

fn run() -> Result<(), Box<dyn Error>> {
    let mut args = std::env::args().skip(1);
    let Some(path) = args.next() else {
        return Err(HELP.into());
    };
    if matches!(path.as_str(), "--help" | "-h") {
        println!("{HELP}");
        return Ok(());
    }
    let id_filter = args.next().ok_or(HELP)?;
    let interface = args.next().unwrap_or_default();
    if args.next().is_some() {
        return Err(HELP.into());
    }
    if id_filter.trim().is_empty() {
        return Err("a CAN ID or PGN filter is required".into());
    }
    let filter = TraceFilter::parse(&interface, &id_filter)?;
    let trace = read_trace(BufReader::new(File::open(&path)?))?;
    let mut assembler = IsoTpAssembler::new();
    let mut inspected = 0_usize;
    let mut reconstructed = 0_usize;
    let mut invalid = 0_usize;
    for (index, frame) in trace.frames.iter().enumerate() {
        if !filter.matches(frame) { continue; }
        inspected += 1;
        match assembler.push(frame) {
            Ok(Some(packet)) => {
                reconstructed += 1;
                let timestamp = packet.end_timestamp_us.map(|us| us.to_string())
                    .unwrap_or_else(|| "-".into());
                let hex = packet.payload.iter()
                    .map(|value| format!("{value:02X}")).collect::<Vec<_>>().join(" ");
                println!("{timestamp} us  {} {}  len={}  {hex}  [{}]",
                    packet.interface, packet.id, packet.payload.len(),
                    describe_payload(&packet.payload));
            }
            Ok(None) => {}
            Err(error) => {
                invalid += 1;
                eprintln!("Trace frame {}: {error}", index + 1);
            }
        }
    }
    eprintln!("Inspected {inspected} matching frames; {reconstructed} ISO-TP payloads; \
        {invalid} invalid fragments; {} unfinished sessions",
        assembler.pending_count());
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
    fn uds_like_labels_do_not_require_diagnostic_writes() {
        assert!(describe_payload(&[0x62, 0xf1, 0x90]).contains("positive"));
        assert!(describe_payload(&[0x7f, 0x22, 0x31]).contains("NRC 31"));
        assert!(describe_payload(&[0x10, 0x03]).contains("not classified"));
        assert_eq!(describe_payload(&[]), "empty payload");
    }
}
