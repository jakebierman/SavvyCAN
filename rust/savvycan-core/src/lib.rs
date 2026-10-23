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
pub mod pcap;

pub use candump::{parse_candump_line, read_candump, CandumpError, TraceReadError};
pub use dbc::{
    ByteOrder, DbcDatabase, DbcMessage, DbcParseError, DbcSignal, DecodeError,
    DecodedMessage, DecodedSignal, MuxCondition, ValueKind,
};
pub use format_rules::{parse_format_rules, RuleParseError};
pub use payload_format::{FormatError, FormattedField, PayloadFormatRouter, PayloadFormatter};
pub use file_formats::{read_trace, write_candump, write_crtd, write_gvret_csv, ExportError, ImportError, ImportedTrace, TraceFormat};
pub use frame::{CanFrame, CanId, Direction, FdFlags, FrameError, FrameKind};
pub use dbc_stack::DbcStack;
pub use signal_export::{write_decoded_csv, SignalExportError};
pub use isotp::{IsoTpAssembler, IsoTpError, IsoTpPacket};
pub use filter::{FilterError, IdFilter, TraceFilter};
pub use receiver::{CanReceive, ReceiveCapabilities, ReceiveDescriptor, ReceiveError, ReplayReceive};
pub use trace::{summarize, TraceSummary};
