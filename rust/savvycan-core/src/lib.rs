//! Rust domain layer for the SavvyCAN migration.
//! No GUI, adapter, or filesystem dependency is required to model frames.

pub mod candump;
pub mod dbc;
pub mod frame;
pub mod file_formats;
pub mod payload_format;
pub mod format_rules;
pub mod trace;
pub mod receiver;
pub mod filter;
pub mod dbc_stack;
pub mod signal_export;
pub mod isotp;
pub mod j1939_bam;
pub mod pcap;
pub mod pcapng;
pub mod trace_stats;
pub mod byte_activity;
pub mod timeseries;
pub mod replay;

pub use candump::{parse_candump_line, read_candump, CandumpError, TraceReadError};
pub use dbc::{
    ByteOrder, DbcDatabase, DbcMessage, DbcParseError, DbcSignal, DecodeError,
    DecodedMessage, DecodedSignal, MuxCondition, ValueKind,
};
pub use format_rules::{parse_format_rules, RuleParseError};
pub use payload_format::{FormatError, FormattedField, PayloadFormatRouter, PayloadFormatter};
pub use file_formats::{read_trace, write_candump, write_crtd, write_gvret_csv, ExportError, ImportError, ImportedTrace, TraceFormat};
pub use frame::{CanFrame, CanId, Direction, FdFlags, FrameError, FrameKind};
pub use dbc_stack::{DbcStack, DbcCatalogEntry};
pub use signal_export::{write_decoded_csv, SignalExportError};
pub use isotp::{describe_uds_like_payload, IsoTpAssembler, IsoTpError, IsoTpPacket};
pub use j1939_bam::{J1939BamAssembler, J1939BamError, J1939BamPacket};
pub use pcap::write_socketcan_pcap;
pub use pcapng::write_socketcan_pcapng;
pub use trace_stats::{analyze_ids, write_id_stats_csv, IdStatistics};
pub use byte_activity::{analyze_byte_activity, analyze_byte_activity_matching, write_byte_activity_csv, write_byte_activity_csv_matching, ByteActivity};
pub use timeseries::{extract_numeric_series, extract_numeric_series_batch, plot_path, plot_path_on_axes, plot_path_on_axes_sorted, HorizontalAxis, NumericSeries, PlotPath};
pub use replay::{OfflineReplay, ReplayClock};
pub use filter::{FilterError, IdFilter, TraceFilter};
pub use receiver::{CanReceive, ReceiveCapabilities, ReceiveDescriptor, ReceiveError, ReplayReceive};
pub use trace::{summarize, TraceSummary};
