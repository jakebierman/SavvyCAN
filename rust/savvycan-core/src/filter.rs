//! Non-destructive CAN-frame filtering for offline and live viewers.
use crate::{CanFrame, CanId};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FilterError(pub String);
impl fmt::Display for FilterError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str(&self.0) }
}
impl std::error::Error for FilterError {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IdFilter {
    Exact(CanId),
    Range { first: CanId, last: CanId },
    Mask { id: CanId, mask: u32 },
    J1939Pgn(u32),
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct TraceFilter {
    pub interface: Option<String>,
    pub id: Option<IdFilter>,
}

fn hex(raw: &str) -> Result<u32, FilterError> {
    let raw = raw.trim();
    let raw = raw.strip_prefix("0x").or_else(|| raw.strip_prefix("0X")).unwrap_or(raw);
    if raw.is_empty() {
        return Err(FilterError("missing hexadecimal ID or mask".into()));
    }
    u32::from_str_radix(raw, 16)
        .map_err(|_| FilterError(format!("invalid hexadecimal CAN ID or mask '{raw}'")))
}

fn typed_id(value: u32, extended: bool) -> Result<CanId, FilterError> {
    if extended {
        CanId::extended(value).map_err(|e| FilterError(e.to_string()))
    } else {
        let narrow = u16::try_from(value)
            .map_err(|_| FilterError("standard CAN ID exceeds 7FF".into()))?;
        CanId::standard(narrow).map_err(|e| FilterError(e.to_string()))
    }
}

fn pgn(id: CanId) -> Option<u32> {
    let CanId::Extended(raw) = id else { return None; };
    let mut group = (raw >> 8) & 0x3ffff;
    if ((raw >> 16) & 0xff) < 240 {
        group &= !0xff;
    }
    Some(group)
}

fn parse_id_filter(raw: &str) -> Result<Option<IdFilter>, FilterError> {
    let id = raw.trim();
    if id.is_empty() { return Ok(None); }
    let upper = id.to_ascii_uppercase();
    if upper.starts_with("PGN:") {
        let group = hex(&id[4..])?;
        if group > 0x3ffff {
            return Err(FilterError("J1939 PGN exceeds 3FFFF".into()));
        }
        if ((group >> 8) & 0xff) < 240 && group & 0xff != 0 {
            return Err(FilterError("J1939 PDU1 PGNs must end in 00".into()));
        }
        return Ok(Some(IdFilter::J1939Pgn(group)));
    }
    let (forced, value) = if upper.starts_with("S:") {
        (Some(false), &id[2..])
    } else if upper.starts_with("X:") {
        (Some(true), &id[2..])
    } else { (None, id) };
    if let Some((start, end)) = value.split_once('-') {
        if end.contains('/') {
            return Err(FilterError("CAN ID range cannot also contain a mask".into()));
        }
        let (first, last) = (hex(start)?, hex(end)?);
        if first > last { return Err(FilterError("CAN ID range start exceeds end".into())); }
        let extended = forced.unwrap_or(last > u32::from(CanId::MAX_STANDARD));
        return Ok(Some(IdFilter::Range {
            first: typed_id(first, extended)?,
            last: typed_id(last, extended)?,
        }));
    }
    if let Some((id, raw_mask)) = value.split_once('/') {
        let (value, mask) = (hex(id)?, hex(raw_mask)?);
        let extended = forced.unwrap_or(
            value > u32::from(CanId::MAX_STANDARD) || mask > u32::from(CanId::MAX_STANDARD)
        );
        let id = typed_id(value, extended)?;
        let maximum = if extended { CanId::MAX_EXTENDED } else { u32::from(CanId::MAX_STANDARD) };
        if mask > maximum { return Err(FilterError("CAN ID mask exceeds identifier width".into())); }
        return Ok(Some(IdFilter::Mask { id, mask }));
    }
    let numeric = hex(value)?;
    let extended = forced.unwrap_or(numeric > u32::from(CanId::MAX_STANDARD));
    Ok(Some(IdFilter::Exact(typed_id(numeric, extended)?)))
}

impl TraceFilter {
    /// Blank fields match all; IDs are hexadecimal. S: and X: force 11/29-bit
    /// IDs; ranges, masked IDs and PGN:xxxx J1939 groups are also accepted.
    pub fn parse(interface: &str, raw_id: &str) -> Result<Self, FilterError> {
        let interface = interface.trim();
        if !interface.is_empty() && !interface.chars().all(|c|
            c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.')) {
            return Err(FilterError("interface contains invalid characters".into()));
        }
        Ok(Self {
            interface: (!interface.is_empty()).then(|| interface.to_owned()),
            id: parse_id_filter(raw_id)?,
        })
    }

    pub fn matches(&self, frame: &CanFrame) -> bool {
        self.interface.as_ref().is_none_or(|iface| &frame.interface == iface)
            && self.id.is_none_or(|query| match query {
                IdFilter::Exact(id) => frame.id == id,
                IdFilter::Range { first, last } =>
                    frame.id.is_extended() == first.is_extended()
                        && frame.id.value() >= first.value()
                        && frame.id.value() <= last.value(),
                IdFilter::Mask { id, mask } =>
                    frame.id.is_extended() == id.is_extended()
                        && (frame.id.value() & mask) == (id.value() & mask),
                IdFilter::J1939Pgn(group) => pgn(frame.id) == Some(group),
            })
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

#[cfg(test)]
mod extended_filter_tests {
    use super::*;
    use crate::{FdFlags, FrameKind};

    fn f(id: CanId) -> CanFrame {
        CanFrame::new(None, "can0", id, FrameKind::Data,
            vec![1], false, FdFlags::default()).unwrap()
    }

    #[test]
    fn masked_and_ranged_ids_preserve_standard_extended_boundary() {
        let range = TraceFilter::parse("", "S:100-1FF").unwrap();
        assert!(range.matches(&f(CanId::Standard(0x123))));
        assert!(!range.matches(&f(CanId::Standard(0x200))));
        assert!(!range.matches(&f(CanId::Extended(0x123))));
        let mask = TraceFilter::parse("", "X:18FF0000/1FFF0000").unwrap();
        assert!(mask.matches(&f(CanId::Extended(0x18ff50e5))));
        assert!(!mask.matches(&f(CanId::Extended(0x18fe50e5))));
        assert!(!mask.matches(&f(CanId::Standard(0x123))));
    }

    #[test]
    fn j1939_pgn_matches_pdu1_and_pdu2_correctly() {
        let pdu2 = TraceFilter::parse("", "PGN:F004").unwrap();
        assert!(pdu2.matches(&f(CanId::Extended(0x0cf00480))));
        assert!(!pdu2.matches(&f(CanId::Extended(0x0cf00380))));
        let pdu1 = TraceFilter::parse("", "PGN:EA00").unwrap();
        assert!(pdu1.matches(&f(CanId::Extended(0x0cea05a0))));
        assert!(pdu1.matches(&f(CanId::Extended(0x18eaffff))));
        assert!(!pdu1.matches(&f(CanId::Standard(0x700))));
    }

    #[test]
    fn invalid_ranges_masks_and_pgns_are_errors() {
        for id in ["S:300-100", "S:100-800", "X:20000000", "S:123/800",
            "X:123-456/1FF", "PGN:40000", "PGN:EA01", "PGN:ZZZZ", "1A/7ZZ"] {
            assert!(TraceFilter::parse("", id).is_err(), "{id} should fail");
        }
    }
}
