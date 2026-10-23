//! Non-destructive, exact CAN frame filtering for offline and live viewers.
use crate::{CanFrame, CanId};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FilterError(pub String);
impl fmt::Display for FilterError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str(&self.0) }
}
impl std::error::Error for FilterError {}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct TraceFilter {
    pub interface: Option<String>,
    pub id: Option<CanId>,
}

impl TraceFilter {
    /// An empty field means any. IDs are hex; S: and X: force 11/29-bit formats.
    pub fn parse(interface: &str, raw_id: &str) -> Result<Self, FilterError> {
        let interface = interface.trim();
        let id = raw_id.trim();
        let id = if id.is_empty() {
            None
        } else {
            let (extended, digits) = if let Some(v) = id.strip_prefix("S:").or_else(|| id.strip_prefix("s:")) {
                (Some(false), v)
            } else if let Some(v) = id.strip_prefix("X:").or_else(|| id.strip_prefix("x:")) {
                (Some(true), v)
            } else { (None, id) };
            let digits = digits.strip_prefix("0x").or_else(|| digits.strip_prefix("0X")).unwrap_or(digits);
            let value = u32::from_str_radix(digits, 16)
                .map_err(|_| FilterError(format!("invalid hexadecimal CAN ID '{id}'")))?;
            let use_extended = extended.unwrap_or(value > CanId::MAX_STANDARD as u32);
            if use_extended {
                Some(CanId::extended(value).map_err(|err| FilterError(err.to_string()))?)
            } else {
                if value > CanId::MAX_STANDARD as u32 {
                    return Err(FilterError("standard CAN ID exceeds 7FF".into()));
                }
                Some(CanId::standard(value as u16).map_err(|err| FilterError(err.to_string()))?)
            }
        };
        if !interface.is_empty() && !interface.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.')) {
            return Err(FilterError("interface contains invalid characters".into()));
        }
        Ok(Self {
            interface: (!interface.is_empty()).then(|| interface.to_owned()),
            id,
        })
    }

    pub fn matches(&self, frame: &CanFrame) -> bool {
        self.interface.as_ref().is_none_or(|iface| &frame.interface == iface)
            && self.id.is_none_or(|id| frame.id == id)
    }

    pub fn is_empty(&self) -> bool { self.interface.is_none() && self.id.is_none() }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{FdFlags, FrameKind};

    fn frame(interface: &str, id: CanId) -> CanFrame {
        CanFrame::new(None, interface, id, FrameKind::Data, vec![1], false, FdFlags::default()).unwrap()
    }

    #[test]
    fn standard_and_extended_identifiers_do_not_collide() {
        let standard = frame("can0", CanId::Standard(0x123));
        let extended = frame("can0", CanId::Extended(0x123));
        let f = TraceFilter::parse("can0", "S:123").unwrap();
        assert!(f.matches(&standard));
        assert!(!f.matches(&extended));
        assert!(!f.matches(&frame("can1", CanId::Standard(0x123))));
        assert!(TraceFilter::parse("", "X:123").unwrap().matches(&extended));
        assert!(TraceFilter::parse("", "18FF50E5").unwrap()
            .matches(&frame("vcan0", CanId::Extended(0x18ff50e5))));
    }

    #[test]
    fn wildcard_filter_and_invalid_inputs() {
        let all = TraceFilter::parse("", "").unwrap();
        assert!(all.is_empty());
        assert!(all.matches(&frame("bus0", CanId::Standard(0))));
        for (interface, id) in [("", "xyz"), ("", "S:800"), ("", "X:20000000"), ("bad/name", "")] {
            assert!(TraceFilter::parse(interface, id).is_err());
        }
    }
}
