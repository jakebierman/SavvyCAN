//! Rust domain layer for the SavvyCAN migration.
//! No GUI, adapter, or filesystem dependency is required to model frames.

pub mod candump;
pub mod dbc;
pub mod frame;
pub mod trace;

pub use candump::{parse_candump_line, read_candump, CandumpError, TraceReadError};
pub use dbc::{
    ByteOrder, DbcDatabase, DbcMessage, DbcParseError, DbcSignal, DecodeError,
    DecodedMessage, DecodedSignal, MuxCondition, ValueKind,
};
pub use frame::{CanFrame, CanId, Direction, FdFlags, FrameError, FrameKind};
pub use trace::{summarize, TraceSummary};
