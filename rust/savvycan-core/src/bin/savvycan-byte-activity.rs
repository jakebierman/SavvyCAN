//! Read-only command-line byte-activity analysis for CAN captures.
use savvycan_core::{read_trace, write_byte_activity_csv, TraceFilter};
use std::error::Error;
use std::fs::File;
use std::io::{self, BufReader};

const HELP: &str = "Usage: savvycan-byte-activity <trace-file> [interface] [CAN-ID-filter]\n\
  Example: savvycan-byte-activity capture.pcapng can0 S:7E8\n\
  Writes per-byte activity CSV to standard output. CAN IDs may be exact,\n\
  ranges, masks, or J1939 PGNs. No adapter is opened or frames transmitted.";

fn run() -> Result<(), Box<dyn Error>> {
    let mut args = std::env::args().skip(1);
    let Some(path) = args.next() else { return Err(HELP.into()); };
    if matches!(path.as_str(), "-h" | "--help") {
        println!("{HELP}");
        return Ok(());
    }
    let interface = args.next().unwrap_or_default();
    let id = args.next().unwrap_or_default();
    if args.next().is_some() { return Err(HELP.into()); }
    let filter = TraceFilter::parse(&interface, &id)?;
    let imported = read_trace(BufReader::new(File::open(path)?))?;
    let selected = imported.frames.into_iter()
        .filter(|frame| filter.matches(frame)).collect::<Vec<_>>();
    let rows = write_byte_activity_csv(io::stdout().lock(), &selected)?;
    eprintln!("Wrote {rows} interface/CAN-ID/byte activity rows from {} frames.",
        selected.len());
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
