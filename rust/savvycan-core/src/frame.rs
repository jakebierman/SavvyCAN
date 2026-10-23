use std::fmt;

/// A CAN arbitration identifier. The format is explicit: 0x123 can be
/// represented as either an 11-bit or a 29-bit identifier.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CanId {
    Standard(u16),
    Extended(u32),
}

impl CanId {
    pub const MAX_STANDARD: u16 = 0x7ff;
    pub const MAX_EXTENDED: u32 = 0x1fff_ffff;

    pub fn standard(id: u16) -> Result<Self, FrameError> {
        if id > Self::MAX_STANDARD {
            return Err(FrameError::InvalidStandardId(id));
        }
        Ok(Self::Standard(id))
    }

    pub fn extended(id: u32) -> Result<Self, FrameError> {
        if id > Self::MAX_EXTENDED {
            return Err(FrameError::InvalidExtendedId(id));
        }
        Ok(Self::Extended(id))
    }

    pub fn value(self) -> u32 {
        match self {
            Self::Standard(id) => id.into(),
            Self::Extended(id) => id,
        }
    }

    pub fn is_extended(self) -> bool {
        matches!(self, Self::Extended(_))
    }
}

impl fmt::Display for CanId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Standard(id) => write!(f, "{id:03X}"),
            Self::Extended(id) => write!(f, "{id:08X}"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    Received,
    Transmitted,
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameKind {
    Data,
    RemoteRequest { dlc: u8 },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FdFlags {
    pub bitrate_switch: bool,
    pub error_state_indicator: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CanFrame {
    /// Microseconds from the trace's timestamp origin; candump -L uses epoch.
    pub timestamp_us: Option<u64>,
    /// Preserve the adapter interface instead of coercing can0/can1 into one bus.
    pub interface: String,
    pub id: CanId,
    pub kind: FrameKind,
    pub data: Vec<u8>,
    pub is_fd: bool,
    pub fd_flags: FdFlags,
    pub direction: Direction,
}

impl CanFrame {
    pub fn new(
        timestamp_us: Option<u64>,
        interface: impl Into<String>,
        id: CanId,
        kind: FrameKind,
        data: Vec<u8>,
        is_fd: bool,
        fd_flags: FdFlags,
    ) -> Result<Self, FrameError> {
        let interface = interface.into();
        if interface.trim().is_empty() {
            return Err(FrameError::MissingInterface);
        }
        if !is_fd && fd_flags != FdFlags::default() {
            return Err(FrameError::FdFlagsOnClassic);
        }
        if is_fd && matches!(kind, FrameKind::RemoteRequest { .. }) {
            return Err(FrameError::FdRemoteRequest);
        }
        if let FrameKind::RemoteRequest { dlc } = kind {
            if dlc > 8 {
                return Err(FrameError::InvalidRemoteDlc(dlc));
            }
            if !data.is_empty() {
                return Err(FrameError::RemoteRequestWithData);
            }
        }
        let max = if is_fd { 64 } else { 8 };
        if data.len() > max {
            return Err(FrameError::PayloadTooLarge { actual: data.len(), max });
        }
        Ok(Self {
            timestamp_us,
            interface,
            id,
            kind,
            data,
            is_fd,
            fd_flags,
            direction: Direction::Unknown,
        })
    }

    pub fn len(&self) -> usize {
        match self.kind {
            FrameKind::Data => self.data.len(),
            FrameKind::RemoteRequest { dlc } => dlc.into(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FrameError {
    InvalidStandardId(u16),
    InvalidExtendedId(u32),
    MissingInterface,
    PayloadTooLarge { actual: usize, max: usize },
    InvalidRemoteDlc(u8),
    RemoteRequestWithData,
    FdRemoteRequest,
    FdFlagsOnClassic,
}

impl fmt::Display for FrameError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidStandardId(id) => write!(f, "standard CAN ID {id:X} exceeds 7FF"),
            Self::InvalidExtendedId(id) => write!(f, "extended CAN ID {id:X} exceeds 1FFFFFFF"),
            Self::MissingInterface => write!(f, "CAN interface name cannot be empty"),
            Self::PayloadTooLarge { actual, max } => write!(f, "{actual} bytes exceeds the {max}-byte payload limit"),
            Self::InvalidRemoteDlc(dlc) => write!(f, "remote request DLC {dlc} exceeds 8"),
            Self::RemoteRequestWithData => write!(f, "remote requests cannot carry payload"),
            Self::FdRemoteRequest => write!(f, "CAN FD does not support remote requests"),
            Self::FdFlagsOnClassic => write!(f, "CAN FD flags require a CAN FD frame"),
        }
    }
}

impl std::error::Error for FrameError {}
