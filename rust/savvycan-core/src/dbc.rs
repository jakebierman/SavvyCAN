//! DBC parsing and signal decoding, kept independent of the Slint UI.
//! The supported subset includes BO_, SG_, VAL_, SIG_VALTYPE_, and SG_MUL_VAL_.
//! Unrecognised metadata is ignored; malformed supported directives are errors.

use std::collections::HashMap;
use std::fmt;

use crate::{CanFrame, CanId, FrameKind};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ByteOrder {
    Intel,
    Motorola,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueKind {
    Integer,
    Float32,
    Float64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MuxCondition {
    pub parent: Option<String>,
    pub ranges: Vec<(u64, u64)>,
}

#[derive(Debug, Clone)]
pub struct DbcSignal {
    pub name: String,
    pub start_bit: usize,
    pub bit_len: u8,
    pub byte_order: ByteOrder,
    pub signed: bool,
    pub factor: f64,
    pub offset: f64,
    pub unit: String,
    pub is_multiplexor: bool,
    pub multiplex: Option<MuxCondition>,
    pub kind: ValueKind,
    pub labels: HashMap<i64, String>,
}

#[derive(Debug, Clone)]
pub struct DbcMessage {
    pub id: CanId,
    pub name: String,
    pub length: usize,
    pub sender: String,
    pub signals: Vec<DbcSignal>,
}

#[derive(Debug, Clone, Default)]
pub struct DbcDatabase {
    pub messages: HashMap<CanId, DbcMessage>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DbcParseError {
    pub line: usize,
    pub detail: String,
}

impl fmt::Display for DbcParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "DBC line {}: {}", self.line, self.detail)
    }
}
impl std::error::Error for DbcParseError {}

fn bad(line: usize, detail: impl Into<String>) -> DbcParseError {
    DbcParseError { line, detail: detail.into() }
}

fn parse_id(text: &str, line: usize) -> Result<CanId, DbcParseError> {
    let value = text.parse::<u32>().map_err(|_| bad(line, "invalid decimal CAN ID"))?;
    if value & 0x8000_0000 != 0 {
        CanId::extended(value & 0x7fff_ffff)
            .map_err(|err| bad(line, err.to_string()))
    } else {
        let value: u16 = value.try_into().map_err(|_| bad(line, "DBC standard ID out of range"))?;
        CanId::standard(value).map_err(|err| bad(line, err.to_string()))
    }
}

fn signal_layout(layout: &str, message_len: usize, line: usize)
    -> Result<(usize, u8, ByteOrder, bool), DbcParseError>
{
    let (start, rest) = layout.split_once('|')
        .ok_or_else(|| bad(line, "missing signal bit separator '|'"))?;
    let (bits, flags) = rest.split_once('@')
        .ok_or_else(|| bad(line, "missing signal byte order '@'"))?;
    let start = start.parse::<usize>().map_err(|_| bad(line, "invalid signal start bit"))?;
    let bits = bits.parse::<u8>().map_err(|_| bad(line, "invalid signal length"))?;
    if bits == 0 || bits > 64 {
        return Err(bad(line, "signal length must be 1..64 bits"));
    }
    let (order, signed) = match flags {
        "1+" => (ByteOrder::Intel, false),
        "1-" => (ByteOrder::Intel, true),
        "0+" => (ByteOrder::Motorola, false),
        "0-" => (ByteOrder::Motorola, true),
        _ => return Err(bad(line, "expected byte order/sign 1+, 1-, 0+ or 0-")),
    };
    let mut position = start;
    for index in 0..usize::from(bits) {
        if position / 8 >= message_len {
            return Err(bad(line, "signal layout extends past the DBC message length"));
        }
        if index + 1 < usize::from(bits) {
            position = match order {
                ByteOrder::Intel => position.checked_add(1),
                ByteOrder::Motorola if position % 8 == 0 => position.checked_add(15),
                ByteOrder::Motorola => position.checked_sub(1),
            }.ok_or_else(|| bad(line, "invalid signal bit position"))?;
        }
    }
    Ok((start, bits, order, signed))
}

fn parse_mux(text: Option<&str>, line: usize)
    -> Result<(bool, Option<MuxCondition>), DbcParseError>
{
    let Some(text) = text else { return Ok((false, None)); };
    if text == "M" {
        return Ok((true, None));
    }
    let value = text.strip_prefix('m').ok_or_else(|| bad(line, "invalid mux indicator"))?;
    let is_mux = value.ends_with('M');
    let number = if is_mux { &value[..value.len() - 1] } else { value };
    let value = number.parse::<u64>().map_err(|_| bad(line, "invalid multiplex value"))?;
    Ok((is_mux, Some(MuxCondition { parent: None, ranges: vec![(value, value)] })))
}

fn parse_signal(line_text: &str, message_len: usize, line: usize)
    -> Result<DbcSignal, DbcParseError>
{
    let (left, right) = line_text.split_once(':')
        .ok_or_else(|| bad(line, "SG_ is missing ':'"))?;
    let mut left = left.trim().split_whitespace();
    let name = left.next().ok_or_else(|| bad(line, "signal name missing"))?;
    let (is_multiplexor, multiplex) = parse_mux(left.next(), line)?;
    if left.next().is_some() {
        return Err(bad(line, "unexpected SG_ name/multiplex tokens"));
    }
    let layout = right.split_whitespace().next()
        .ok_or_else(|| bad(line, "signal bit layout missing"))?;
    let (start_bit, bit_len, byte_order, signed) = signal_layout(layout, message_len, line)?;
    let open = right.find('(').ok_or_else(|| bad(line, "signal scale missing"))?;
    let close = right[open + 1..].find(')')
        .map(|pos| open + pos + 1)
        .ok_or_else(|| bad(line, "signal scale not closed"))?;
    let (factor, offset) = right[open + 1..close].split_once(',')
        .ok_or_else(|| bad(line, "signal scale requires factor,offset"))?;
    let factor = factor.trim().parse::<f64>().map_err(|_| bad(line, "invalid scale factor"))?;
    let offset = offset.trim().parse::<f64>().map_err(|_| bad(line, "invalid scale offset"))?;
    if !factor.is_finite() || !offset.is_finite() {
        return Err(bad(line, "non-finite scale value"));
    }
    let unit_start = right[close + 1..].find('"')
        .map(|pos| close + 1 + pos).ok_or_else(|| bad(line, "unit quote missing"))?;
    let unit_end = right[unit_start + 1..].find('"')
        .map(|pos| unit_start + 1 + pos).ok_or_else(|| bad(line, "unit quote not closed"))?;
    Ok(DbcSignal {
        name: name.to_owned(),
        start_bit, bit_len, byte_order, signed, factor, offset,
        unit: right[unit_start + 1..unit_end].into(),
        is_multiplexor, multiplex,
        kind: ValueKind::Integer,
        labels: HashMap::new(),
    })
}

fn parse_ranges(text: &str, line: usize) -> Result<Vec<(u64, u64)>, DbcParseError> {
    let mut result = Vec::new();
    for item in text.trim_end_matches(';').split(',') {
        let (low, high) = item.trim().split_once('-')
            .ok_or_else(|| bad(line, "extended mux range must be min-max"))?;
        let low = low.trim().parse::<u64>().map_err(|_| bad(line, "invalid mux range start"))?;
        let high = high.trim().parse::<u64>().map_err(|_| bad(line, "invalid mux range end"))?;
        if high < low {
            return Err(bad(line, "multiplex range end precedes start"));
        }
        result.push((low, high));
    }
    if result.is_empty() {
        return Err(bad(line, "extended mux range cannot be empty"));
    }
    Ok(result)
}

impl DbcDatabase {
    pub fn parse(source: &str) -> Result<Self, DbcParseError> {
        let mut result = Self::default();
        let mut current = None;
        for (index, raw_line) in source.lines().enumerate() {
            let line = index + 1;
            let entry = raw_line.trim();
            if let Some(rest) = entry.strip_prefix("BO_ ") {
                let mut words = rest.split_whitespace();
                let id = parse_id(words.next().ok_or_else(|| bad(line, "missing message ID"))?, line)?;
                let name = words.next().ok_or_else(|| bad(line, "missing message name"))?
                    .strip_suffix(':').ok_or_else(|| bad(line, "message name must end with ':'"))?;
                let length = words.next().ok_or_else(|| bad(line, "missing DLC"))?
                    .parse::<usize>().map_err(|_| bad(line, "invalid DLC"))?;
                if length > 64 {
                    return Err(bad(line, "DBC message length exceeds 64 bytes"));
                }
                let sender = words.next().unwrap_or("Vector__XXX");
                if result.messages.contains_key(&id) {
                    return Err(bad(line, format!("duplicate message ID {}", id)));
                }
                result.messages.insert(id, DbcMessage {
                    id, name: name.into(), length, sender: sender.into(), signals: Vec::new(),
                });
                current = Some(id);
            } else if let Some(rest) = entry.strip_prefix("SG_ ") {
                let id = current.ok_or_else(|| bad(line, "SG_ precedes the first BO_"))?;
                let message = result.messages.get_mut(&id).expect("current BO_ exists");
                let signal = parse_signal(rest, message.length, line)?;
                if message.signals.iter().any(|existing| existing.name == signal.name) {
                    return Err(bad(line, "duplicate signal name"));
                }
                message.signals.push(signal);
            } else if let Some(rest) = entry.strip_prefix("VAL_ ") {
                let mut tokens = rest.split_whitespace();
                let id = parse_id(tokens.next().ok_or_else(|| bad(line, "missing VAL_ ID"))?, line)?;
                let name = tokens.next().ok_or_else(|| bad(line, "missing VAL_ signal"))?;
                let values = rest.split_once(name).map(|(_, values)| values)
                    .ok_or_else(|| bad(line, "malformed VAL_"))?;
                let signal = result.messages.get_mut(&id)
                    .and_then(|message| message.signals.iter_mut().find(|signal| signal.name == name))
                    .ok_or_else(|| bad(line, "VAL_ references an unknown signal"))?;
                let mut values = values.trim();
                while !values.is_empty() && values != ";" {
                    let open = values.find('"').ok_or_else(|| bad(line, "VAL_ label quote missing"))?;
                    let value = values[..open].trim().parse::<i64>()
                        .map_err(|_| bad(line, "invalid VAL_ numeric key"))?;
                    values = &values[open + 1..];
                    let close = values.find('"').ok_or_else(|| bad(line, "VAL_ label quote not closed"))?;
                    let label = &values[..close];
                    if signal.labels.insert(value, label.into()).is_some() {
                        return Err(bad(line, "duplicate VAL_ key"));
                    }
                    values = values[close + 1..].trim();
                    values = values.strip_prefix(';').unwrap_or(values).trim();
                }
            } else if let Some(rest) = entry.strip_prefix("SIG_VALTYPE_ ") {
                let (target, kind) = rest.split_once(':')
                    .ok_or_else(|| bad(line, "SIG_VALTYPE_ requires ':'"))?;
                let mut target = target.split_whitespace();
                let id = parse_id(target.next().ok_or_else(|| bad(line, "missing SIG_VALTYPE_ ID"))?, line)?;
                let name = target.next().ok_or_else(|| bad(line, "missing SIG_VALTYPE_ signal"))?;
                let kind = match kind.trim().trim_end_matches(';').trim() {
                    "1" => ValueKind::Float32,
                    "2" => ValueKind::Float64,
                    _ => return Err(bad(line, "unsupported SIG_VALTYPE_")),
                };
                let signal = result.messages.get_mut(&id)
                    .and_then(|message| message.signals.iter_mut().find(|signal| signal.name == name))
                    .ok_or_else(|| bad(line, "SIG_VALTYPE_ references unknown signal"))?;
                let expected_width = if kind == ValueKind::Float32 { 32 } else { 64 };
                if signal.bit_len != expected_width {
                    return Err(bad(line, "floating point signal width must be 32 or 64"));
                }
                signal.kind = kind;
            } else if let Some(rest) = entry.strip_prefix("SG_MUL_VAL_ ") {
                let mut words = rest.split_whitespace();
                let id = parse_id(words.next().ok_or_else(|| bad(line, "missing mux message ID"))?, line)?;
                let name = words.next().ok_or_else(|| bad(line, "missing mux signal"))?;
                let parent = words.next().ok_or_else(|| bad(line, "missing mux parent"))?;
                let ranges = parse_ranges(&words.collect::<Vec<_>>().join(""), line)?;
                let message = result.messages.get_mut(&id)
                    .ok_or_else(|| bad(line, "SG_MUL_VAL_ references unknown message"))?;
                if !message.signals.iter().any(|signal| signal.name == parent && signal.is_multiplexor) {
                    return Err(bad(line, "SG_MUL_VAL_ parent is not a multiplexor"));
                }
                let signal = message.signals.iter_mut().find(|signal| signal.name == name)
                    .ok_or_else(|| bad(line, "SG_MUL_VAL_ references unknown signal"))?;
                signal.multiplex = Some(MuxCondition { parent: Some(parent.into()), ranges });
            }
        }
        Ok(result)
    }

    pub fn decode(&self, frame: &CanFrame) -> Result<Option<DecodedMessage>, DecodeError> {
        if !matches!(frame.kind, FrameKind::Data) {
            return Ok(None);
        }
        let Some(message) = self.messages.get(&frame.id) else {
            return Ok(None);
        };
        let mut decoded = Vec::new();
        for signal in &message.signals {
            if !message.active(signal, &frame.data, &mut Vec::new())? {
                continue;
            }
            let raw = extract_bits(signal, &frame.data)?;
            let signed_raw = if signal.signed { sign_extend(raw, signal.bit_len) }
                else { raw as i64 };
            let numeric = match signal.kind {
                ValueKind::Integer if signal.signed => signed_raw as f64,
                ValueKind::Integer => raw as f64,
                ValueKind::Float32 => f32::from_bits(raw as u32) as f64,
                ValueKind::Float64 => f64::from_bits(raw),
            };
            let value = numeric * signal.factor + signal.offset;
            let text = signal.labels.get(&signed_raw).cloned();
            decoded.push(DecodedSignal {
                name: signal.name.clone(),
                raw, value, unit: signal.unit.clone(), text,
            });
        }
        Ok(Some(DecodedMessage { name: message.name.clone(), signals: decoded }))
    }
}

impl DbcMessage {
    fn active(&self, signal: &DbcSignal, data: &[u8], path: &mut Vec<String>)
        -> Result<bool, DecodeError>
    {
        let Some(mux) = &signal.multiplex else { return Ok(true); };
        if path.contains(&signal.name) {
            return Err(DecodeError::MultiplexCycle(signal.name.clone()));
        }
        path.push(signal.name.clone());
        let parent = if let Some(name) = &mux.parent {
            self.signals.iter().find(|candidate| &candidate.name == name)
        } else {
            self.signals.iter().find(|candidate| candidate.is_multiplexor && candidate.multiplex.is_none())
        }.ok_or_else(|| DecodeError::MissingMultiplexor(signal.name.clone()))?;
        if !parent.is_multiplexor {
            return Err(DecodeError::MissingMultiplexor(signal.name.clone()));
        }
        let parent_active = self.active(parent, data, path)?;
        path.pop();
        if !parent_active { return Ok(false); }
        let raw = extract_bits(parent, data)?;
        Ok(mux.ranges.iter().any(|(low, high)| *low <= raw && raw <= *high))
    }
}

fn extract_bits(signal: &DbcSignal, data: &[u8]) -> Result<u64, DecodeError> {
    let mut position = signal.start_bit;
    let mut raw = 0_u64;
    for bit_index in 0..usize::from(signal.bit_len) {
        let byte = data.get(position / 8)
            .ok_or_else(|| DecodeError::SignalOutOfBounds(signal.name.clone()))?;
        let bit = (u64::from(*byte) >> (position % 8)) & 1;
        match signal.byte_order {
            ByteOrder::Intel => raw |= bit << bit_index,
            ByteOrder::Motorola => raw = (raw << 1) | bit,
        }
        if bit_index + 1 < usize::from(signal.bit_len) {
            position = match signal.byte_order {
                ByteOrder::Intel => position + 1,
                ByteOrder::Motorola if position % 8 == 0 => position + 15,
                ByteOrder::Motorola => position - 1,
            };
        }
    }
    Ok(raw)
}

fn sign_extend(raw: u64, width: u8) -> i64 {
    let shift = 64 - u32::from(width);
    ((raw << shift) as i64) >> shift
}

#[derive(Debug, Clone, PartialEq)]
pub struct DecodedSignal {
    pub name: String,
    pub raw: u64,
    pub value: f64,
    pub unit: String,
    pub text: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct DecodedMessage {
    pub name: String,
    pub signals: Vec<DecodedSignal>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DecodeError {
    MissingMultiplexor(String),
    MultiplexCycle(String),
    SignalOutOfBounds(String),
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::MissingMultiplexor(name) => write!(f, "signal {name} has no valid multiplexor"),
            Self::MultiplexCycle(name) => write!(f, "multiplex dependency cycle at {name}"),
            Self::SignalOutOfBounds(name) => write!(f, "signal {name} exceeds the captured payload"),
        }
    }
}
impl std::error::Error for DecodeError {}
