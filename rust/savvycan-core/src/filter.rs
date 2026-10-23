//! Non-destructive CAN-frame filtering for offline and live viewers.
use crate::{CanFrame, CanId, FrameKind};
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
    pub from_us: Option<u64>,
    pub through_us: Option<u64>,
    /// Optional contiguous byte pattern; None elements match any byte.
    pub payload: Option<Vec<Option<u8>>>,
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


/// Parse a non-negative seconds timestamp with at most six fractional digits.
/// Avoid floating-point rounding of capture microseconds.
fn parse_time(raw: &str) -> Result<Option<u64>, FilterError> {
    let raw = raw.trim();
    if raw.is_empty() { return Ok(None); }
    let (whole, fraction) = raw.split_once('.').unwrap_or((raw, ""));
    if whole.is_empty() || !whole.bytes().all(|c| c.is_ascii_digit())
        || (!fraction.is_empty() && !fraction.bytes().all(|c| c.is_ascii_digit()))
        || fraction.len() > 6 || raw.ends_with('.') || raw.matches('.').count() > 1
    {
        return Err(FilterError(format!(
            "invalid timestamp '{raw}': enter non-negative seconds with up to six decimals"
        )));
    }
    let seconds = whole.parse::<u64>()
        .map_err(|_| FilterError("timestamp exceeds u64 range".into()))?;
    let millis = if fraction.is_empty() { 0 } else {
        fraction.parse::<u64>().map_err(|_| FilterError("invalid timestamp fraction".into()))?
            * 10_u64.pow(6 - fraction.len() as u32)
    };
    seconds.checked_mul(1_000_000).and_then(|us| us.checked_add(millis))
        .map(Some).ok_or_else(|| FilterError("timestamp exceeds u64 microsecond range".into()))
}

/// Parse a payload subsequence as byte pairs, with optional ?? wildcard bytes.
/// Whitespace, commas and underscores are ignored: "22 F1 ??", "22F1??".
fn parse_payload(raw: &str) -> Result<Option<Vec<Option<u8>>>, FilterError> {
    let stripped: String = raw.chars()
        .filter(|character| !character.is_ascii_whitespace() && *character != ','
            && *character != '_')
        .collect();
    if stripped.is_empty() { return Ok(None); }
    if stripped.len() % 2 != 0 {
        return Err(FilterError("payload filter requires complete hex byte pairs (or ??)".into()));
    }
    if stripped.len() > 128 {
        return Err(FilterError("payload pattern exceeds 64 CAN FD bytes".into()));
    }
    let bytes = stripped.as_bytes();
    let mut pattern = Vec::with_capacity(bytes.len() / 2);
    for pair in bytes.chunks_exact(2) {
        if pair == b"??" {
            pattern.push(None);
        } else {
            let pair = std::str::from_utf8(pair)
                .map_err(|_| FilterError("payload pattern must contain ASCII hex or ??".into()))?;
            let byte = u8::from_str_radix(pair, 16)
                .map_err(|_| FilterError(format!("invalid payload hex byte '{pair}'")))?;
            pattern.push(Some(byte));
        }
    }
    Ok(Some(pattern))
}

impl TraceFilter {
    /// Blank fields match all; IDs are hexadecimal. S: and X: force 11/29-bit
    /// IDs; ranges, masked IDs and PGN:xxxx J1939 groups are also accepted.
    pub fn parse(interface: &str, raw_id: &str) -> Result<Self, FilterError> {
        Self::parse_with_time(interface, raw_id, "", "")
    }

    /// Optional inclusive seconds bounds are parsed exactly to microseconds.
    /// Frames lacking timestamps do not match a bounded time search.
    pub fn parse_with_time(interface: &str, raw_id: &str, from: &str, through: &str)
        -> Result<Self, FilterError>
    {
        Self::parse_with_time_and_payload(interface, raw_id, from, through, "")
    }

    /// Hex byte subsequences are matched against data frames only. Each
    /// wildcard ?? consumes exactly one payload byte.
    pub fn parse_with_time_and_payload(
        interface: &str, raw_id: &str, from: &str, through: &str, payload: &str,
    ) -> Result<Self, FilterError> {
        let payload = parse_payload(payload)?;
        let from_us = parse_time(from)?;
        let through_us = parse_time(through)?;
        if from_us.zip(through_us).is_some_and(|(start, end)| start > end) {
            return Err(FilterError("start time exceeds end time".into()));
        }
        let interface = interface.trim();
        if !interface.is_empty() && !interface.chars().all(|c|
            c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.')) {
            return Err(FilterError("interface contains invalid characters".into()));
        }
        Ok(Self {
            interface: (!interface.is_empty()).then(|| interface.to_owned()),
            id: parse_id_filter(raw_id)?,
            from_us,
            through_us,
            payload,
        })
    }

    pub fn matches(&self, frame: &CanFrame) -> bool {
        (self.from_us.is_none() && self.through_us.is_none()
            || frame.timestamp_us.is_some_and(|timestamp|
                self.from_us.is_none_or(|start| timestamp >= start)
                && self.through_us.is_none_or(|end| timestamp <= end)))
            && self.interface.as_ref().is_none_or(|iface| &frame.interface == iface)
            && self.payload.as_ref().is_none_or(|pattern| {
                matches!(frame.kind, FrameKind::Data) &&
                    frame.data.windows(pattern.len()).any(|window| {
                        pattern.iter().zip(window).all(|(target, actual)|
                            target.is_none_or(|byte| byte == *actual))
                    })
            })
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

    pub fn is_empty(&self) -> bool { self.interface.is_none() && self.id.is_none() && self.from_us.is_none() && self.through_us.is_none() && self.payload.is_none() }
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

#[cfg(test)]
mod timestamp_filter_tests {
    use super::*;
    use crate::{FdFlags, FrameKind};

    fn frame(time: Option<u64>) -> CanFrame {
        CanFrame::new(time, "can0", CanId::Standard(0x123), FrameKind::Data,
            vec![1], false, FdFlags::default()).unwrap()
    }

    #[test]
    fn inclusive_microsecond_bounds_and_unknown_timestamp_policy() {
        let filter = TraceFilter::parse_with_time("can0", "S:123", "1.000001", "2.2").unwrap();
        assert!(!filter.matches(&frame(Some(1_000_000))));
        assert!(filter.matches(&frame(Some(1_000_001))));
        assert!(filter.matches(&frame(Some(2_200_000))));
        assert!(!filter.matches(&frame(Some(2_200_001))));
        assert!(!filter.matches(&frame(None)));
        assert!(!filter.is_empty());
        assert!(TraceFilter::parse_with_time("", "", "", "").unwrap().matches(&frame(None)));
    }

    #[test]
    fn time_filter_rejects_imprecise_negative_and_overflow_inputs() {
        for raw in ["-1", ".2", "2.", "1.0000001", "1e2", "1,2",
            "12.abc", "18446744073709551615", "1.2.3"] {
            assert!(TraceFilter::parse_with_time("", "", raw, "").is_err(), "{raw}");
        }
        assert!(TraceFilter::parse_with_time("", "", "5.000001", "5").is_err());
        assert_eq!(parse_time("0.000010").unwrap(), Some(10));
        assert_eq!(parse_time("10").unwrap(), Some(10_000_000));
    }
}

#[cfg(test)]
mod payload_filter_tests {
    use super::*;
    use crate::{FdFlags, FrameKind};

    fn frame(kind: FrameKind, bytes: Vec<u8>, is_fd: bool) -> CanFrame {
        CanFrame::new(Some(10), "can0", CanId::Standard(0x123),
            kind, bytes, is_fd, FdFlags::default()).unwrap()
    }

    #[test]
    fn matching_subsequence_and_wildcards_respect_length_and_frame_kind() {
        let filter = TraceFilter::parse_with_time_and_payload(
            "can0", "S:123", "", "", "22 f1 ?? 90").unwrap();
        assert!(filter.matches(&frame(FrameKind::Data,
            vec![0x00, 0x22, 0xf1, 0xa0, 0x90, 0xff], false)));
        assert!(!filter.matches(&frame(FrameKind::Data,
            vec![0x22, 0xf1, 0x90], false)));
        assert!(!filter.matches(&frame(FrameKind::Data,
            vec![0x22, 0xf1, 0x80, 0x91], false)));
        assert!(!filter.matches(&frame(FrameKind::RemoteRequest { dlc: 4 },
            vec![], false)));
        let exact = TraceFilter::parse_with_time_and_payload("", "", "", "", "22F1A090")
            .unwrap();
        assert!(exact.matches(&frame(FrameKind::Data,
            vec![0x00, 0x22, 0xf1, 0xa0, 0x90, 0xff], true)));
        assert!(!exact.is_empty());
    }

    #[test]
    fn byte_pattern_parser_rejects_invalid_and_unbounded_patterns() {
        for raw in ["A", "?0", "0?", "GG", "00-11", "00 XX", "0??"] {
            assert!(parse_payload(raw).is_err(), "{raw}");
        }
        assert!(parse_payload(&"aa".repeat(65)).is_err());
        assert_eq!(parse_payload("  ").unwrap(), None);
        assert_eq!(parse_payload("01,??_fF").unwrap(),
            Some(vec![Some(1), None, Some(255)]));
    }
}
