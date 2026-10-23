//! SavvyCAN-compatible custom payload formatting, independent of DBC decoding.
use crate::{CanFrame, CanId, FrameKind};
use std::collections::HashMap;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FormatError(pub String);

impl fmt::Display for FormatError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str(&self.0) }
}
impl std::error::Error for FormatError {}

fn invalid(message: impl Into<String>) -> FormatError { FormatError(message.into()) }

#[derive(Debug, Clone, Copy)]
enum BitStyle { Numeric, Boolean, Flag }

#[derive(Debug, Clone)]
enum FieldType {
    Integer { bytes: usize, signed: bool, little: bool },
    Float { bytes: usize, little: bool },
    Text { bytes: usize, utf8: bool },
    Bit { bit: u8, style: BitStyle },
}

impl FieldType {
    fn bytes(&self) -> usize {
        match self {
            Self::Integer { bytes, .. } | Self::Float { bytes, .. } | Self::Text { bytes, .. } => *bytes,
            Self::Bit { .. } => 1,
        }
    }
    fn width(&self) -> usize { self.bytes() * 8 }
}

#[derive(Debug, Clone)]
struct EnumLabels {
    labels: HashMap<u64, String>,
    fallback: Option<String>,
}

#[derive(Debug, Clone)]
struct Field {
    name: Option<String>,
    kind: FieldType,
    offset: usize,
    explicit_offset: bool,
    mask: Option<u64>,
    shift: Option<(bool, u32)>, // true = left
    multiplier: f64,
    divisor: f64,
    addition: f64,
    arithmetic: bool,
    unit: String,
    precision: Option<usize>,
    enums: Option<EnumLabels>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FormattedField {
    pub name: Option<String>,
    pub value: String,
    pub unit: String,
}

impl FormattedField {
    pub fn display(&self) -> String {
        let value = match &self.name {
            Some(name) => format!("{name}={}", self.value),
            None => self.value.clone(),
        };
        if self.unit.is_empty() { value } else { format!("{value} {}", self.unit) }
    }
}

#[derive(Debug, Clone)]
pub struct PayloadFormatter {
    fields: Vec<Field>,
    repeat_single_field: bool,
}

impl PayloadFormatter {
    pub fn compile(expression: &str, repeat_single_field: bool) -> Result<Self, FormatError> {
        let mut next_offset = 0usize;
        let mut fields = Vec::new();
        for token in expression.split_whitespace() {
            let field = parse_field(token, &mut next_offset)?;
            fields.push(field);
        }
        if fields.is_empty() { return Err(invalid("format must contain at least one field")); }
        if repeat_single_field && fields.len() != 1 {
            return Err(invalid("repeat mode requires exactly one field"));
        }
        if repeat_single_field && fields[0].explicit_offset {
            return Err(invalid("repeat mode cannot use an explicit byte offset"));
        }
        Ok(Self { fields, repeat_single_field })
    }

    pub fn format_fields(&self, payload: &[u8]) -> Vec<FormattedField> {
        if self.repeat_single_field {
            let field = &self.fields[0];
            return (0..payload.len()).step_by(field.kind.bytes()).map(|offset| {
                FormattedField {
                    name: None,
                    value: evaluate(field, payload, offset),
                    unit: field.unit.clone(),
                }
            }).collect();
        }
        self.fields.iter().map(|field| FormattedField {
            name: field.name.clone(),
            value: evaluate(field, payload, field.offset),
            unit: field.unit.clone(),
        }).collect()
    }

    pub fn format(&self, payload: &[u8]) -> String {
        self.format_fields(payload).iter().map(FormattedField::display)
            .collect::<Vec<_>>().join(" ")
    }

    pub fn warnings(&self, payload_len: usize) -> Vec<String> {
        if self.repeat_single_field { return Vec::new(); }
        self.fields.iter().filter_map(|field| {
            let end = field.offset.checked_add(field.kind.bytes())?;
            if end <= payload_len { return None; }
            let name = field.name.as_deref().unwrap_or("field");
            Some(format!(
                "{name} requires bytes {}-{}, but payload has {payload_len} bytes",
                field.offset, end - 1
            ))
        }).collect()
    }
}

fn parse_kind(name: &str) -> Result<FieldType, FormatError> {
    if let Some(suffix) = name.strip_prefix("ascii").or_else(|| name.strip_prefix("utf8"))
        .or_else(|| name.strip_prefix("str")) {
        let bytes = suffix.parse::<usize>().map_err(|_| invalid("text type requires a byte count"))?;
        if bytes == 0 || bytes > 4096 { return Err(invalid("text field length must be 1..4096")); }
        return Ok(FieldType::Text { bytes, utf8: !name.starts_with("ascii") });
    }
    for (prefix, style) in [("bit", BitStyle::Numeric), ("bool", BitStyle::Boolean), ("flag", BitStyle::Flag)] {
        if let Some(suffix) = name.strip_prefix(prefix) {
            let bit = suffix.parse::<u8>().map_err(|_| invalid("bit type requires a bit index 0..7"))?;
            if bit > 7 { return Err(invalid("bit index must be 0..7")); }
            return Ok(FieldType::Bit { bit, style });
        }
    }
    if matches!(name, "u8" | "i8") {
        return Ok(FieldType::Integer { bytes: 1, signed: name == "i8", little: true });
    }
    if name.len() >= 4 && matches!(&name[0..1], "u" | "i") {
        let signed = name.starts_with('i');
        let bytes = match &name[1..name.len()-2] {
            "16" => 2, "32" => 4, "64" => 8, _ => 0,
        };
        let endian = &name[name.len()-2..];
        if bytes != 0 && matches!(endian, "le" | "be") {
            return Ok(FieldType::Integer { bytes, signed, little: endian == "le" });
        }
    }
    let float = match name {
        "f32le" => Some((4, true)), "f32be" => Some((4, false)),
        "f64le" => Some((8, true)), "f64be" => Some((8, false)), _ => None,
    };
    if let Some((bytes, little)) = float { return Ok(FieldType::Float { bytes, little }); }
    Err(invalid(format!("unknown field type: {name}")))
}

fn take_decimal(cursor: &mut &str, what: &str) -> Result<usize, FormatError> {
    let end = cursor.find(|c: char| !c.is_ascii_digit()).unwrap_or(cursor.len());
    if end == 0 { return Err(invalid(format!("{what} requires an unsigned integer"))); }
    let result = cursor[..end].parse::<usize>().map_err(|_| invalid(format!("{what} is too large")))?;
    *cursor = &cursor[end..];
    Ok(result)
}

fn take_unsigned(cursor: &mut &str, what: &str) -> Result<u64, FormatError> {
    let hexadecimal = cursor.starts_with("0x") || cursor.starts_with("0X");
    if hexadecimal {
        *cursor = &cursor[2..];
        let end = cursor.find(|c: char| !c.is_ascii_hexdigit()).unwrap_or(cursor.len());
        if end == 0 { return Err(invalid(format!("{what} requires hex digits"))); }
        let result = u64::from_str_radix(&cursor[..end], 16)
            .map_err(|_| invalid(format!("{what} is outside u64 range")))?;
        *cursor = &cursor[end..];
        Ok(result)
    } else {
        let end = cursor.find(|c: char| !c.is_ascii_digit()).unwrap_or(cursor.len());
        if end == 0 { return Err(invalid(format!("{what} requires digits"))); }
        let result = cursor[..end].parse::<u64>()
            .map_err(|_| invalid(format!("{what} is outside u64 range")))?;
        *cursor = &cursor[end..];
        Ok(result)
    }
}

fn take_float(cursor: &mut &str, what: &str) -> Result<f64, FormatError> {
    let bytes = cursor.as_bytes();
    let mut end = 0;
    if bytes.get(end).is_some_and(|c| *c == b'+' || *c == b'-') { end += 1; }
    let mut digits = 0;
    while bytes.get(end).is_some_and(u8::is_ascii_digit) { end += 1; digits += 1; }
    if bytes.get(end) == Some(&b'.') {
        end += 1;
        while bytes.get(end).is_some_and(u8::is_ascii_digit) { end += 1; digits += 1; }
    }
    if digits == 0 { return Err(invalid(format!("{what} requires a number"))); }
    if bytes.get(end).is_some_and(|c| *c == b'e' || *c == b'E') {
        let start = end;
        end += 1;
        if bytes.get(end).is_some_and(|c| *c == b'+' || *c == b'-') { end += 1; }
        let exp_start = end;
        while bytes.get(end).is_some_and(u8::is_ascii_digit) { end += 1; }
        if exp_start == end { end = start; }
    }
    let value = cursor[..end].parse::<f64>().map_err(|_| invalid(format!("{what} is invalid")))?;
    if !value.is_finite() { return Err(invalid(format!("{what} must be finite"))); }
    *cursor = &cursor[end..];
    Ok(value)
}

fn parse_enums(contents: &str) -> Result<EnumLabels, FormatError> {
    let mut labels = HashMap::new();
    let mut fallback = None;
    if contents.is_empty() { return Err(invalid("enum must contain entries")); }
    for item in contents.split(',') {
        let (key, label) = item.split_once(':').ok_or_else(|| invalid("invalid enum entry"))?;
        if label.is_empty() || label.chars().any(|c| matches!(c, '{' | '}' | ':')) {
            return Err(invalid("invalid enum label"));
        }
        if key == "*" {
            if fallback.replace(label.to_string()).is_some() {
                return Err(invalid("duplicate enum fallback"));
            }
        } else {
            let parsed = if let Some(raw) = key.strip_prefix("-0x") {
                let n = i64::from_str_radix(raw, 16).map_err(|_| invalid("invalid negative enum key"))?;
                n.checked_neg().ok_or_else(|| invalid("enum key outside i64 range"))? as u64
            } else if let Some(raw) = key.strip_prefix("0x") {
                u64::from_str_radix(raw, 16).map_err(|_| invalid("invalid hex enum key"))?
            } else if key.starts_with('-') {
                key.parse::<i64>().map_err(|_| invalid("invalid signed enum key"))? as u64
            } else {
                key.parse::<u64>().map_err(|_| invalid("invalid enum key"))?
            };
            if labels.insert(parsed, label.to_string()).is_some() {
                return Err(invalid("duplicate enum value"));
            }
        }
    }
    Ok(EnumLabels { labels, fallback })
}

fn parse_field(token: &str, next_offset: &mut usize) -> Result<Field, FormatError> {
    let type_boundary = token.find(|c: char| matches!(c, '@' | '&' | '<' | '>' | '*' | '/' | '+' | '-' | '[' | '{'))
        .unwrap_or(token.len());
    let header = &token[..type_boundary];
    let (name, type_str) = if let Some((name, kind)) = header.split_once(':') {
        let valid = !name.is_empty() && name.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_')
            && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        if !valid { return Err(invalid(format!("invalid field name: {name}"))); }
        (Some(name.to_string()), kind)
    } else { (None, header) };
    let kind = parse_kind(type_str)?;
    let mut cursor = &token[type_boundary..];
    let explicit_offset = cursor.starts_with('@');
    let offset = if explicit_offset {
        cursor = &cursor[1..];
        take_decimal(&mut cursor, "byte offset")?
    } else { *next_offset };
    *next_offset = offset.checked_add(kind.bytes()).ok_or_else(|| invalid("field offset overflow"))?;

    let mask = if cursor.starts_with('&') {
        cursor = &cursor[1..];
        Some(take_unsigned(&mut cursor, "mask")?)
    } else { None };
    let shift = if cursor.starts_with(">>") || cursor.starts_with("<<") {
        let left = cursor.starts_with("<<");
        cursor = &cursor[2..];
        let count = take_decimal(&mut cursor, "shift")?;
        if count >= kind.width() { return Err(invalid("shift exceeds field width")); }
        Some((left, count as u32))
    } else { None };
    let mut multiplier = 1.0;
    let mut divisor = 1.0;
    let mut addition = 0.0;
    let mut arithmetic = false;
    if cursor.starts_with('*') {
        cursor = &cursor[1..];
        multiplier = take_float(&mut cursor, "multiplier")?;
        arithmetic = true;
    }
    if cursor.starts_with('/') {
        cursor = &cursor[1..];
        divisor = take_float(&mut cursor, "divisor")?;
        if divisor == 0.0 { return Err(invalid("divisor cannot be zero")); }
        arithmetic = true;
    }
    if cursor.starts_with('+') || cursor.starts_with('-') {
        addition = take_float(&mut cursor, "additive offset")?;
        arithmetic = true;
    }

    let unit = if cursor.starts_with('[') {
        cursor = &cursor[1..];
        let end = cursor.find(']').ok_or_else(|| invalid("unclosed unit bracket"))?;
        let unit = cursor[..end].to_string();
        cursor = &cursor[end + 1..];
        unit
    } else { String::new() };

    let mut precision = None;
    let mut enums = None;
    for _ in 0..2 {
        if !cursor.starts_with('{') { break; }
        cursor = &cursor[1..];
        let end = cursor.find('}').ok_or_else(|| invalid("unclosed brace"))?;
        let content = &cursor[..end];
        if content.contains(':') {
            if enums.is_some() { return Err(invalid("duplicate enum block")); }
            enums = Some(parse_enums(content)?);
        } else {
            if precision.is_some() || enums.is_some() { return Err(invalid("misplaced precision block")); }
            let n = content.parse::<usize>().map_err(|_| invalid("precision must be a decimal integer"))?;
            if n > 15 { return Err(invalid("precision exceeds maximum of 15")); }
            precision = Some(n);
        }
        cursor = &cursor[end + 1..];
    }
    if !cursor.is_empty() { return Err(invalid(format!("unexpected modifier in {token}: {cursor}"))); }

    match kind {
        FieldType::Text { .. } if mask.is_some() || shift.is_some() || arithmetic || !unit.is_empty()
            || precision.is_some() || enums.is_some() =>
            return Err(invalid("text fields only support names and offsets")),
        FieldType::Bit { .. } if mask.is_some() || shift.is_some() || arithmetic || !unit.is_empty()
            || precision.is_some() =>
            return Err(invalid("bit fields only support names, offsets and enums")),
        FieldType::Float { .. } if mask.is_some() || shift.is_some() || enums.is_some() =>
            return Err(invalid("floating-point fields do not support masks, shifts or enums")),
        _ => {}
    }
    if arithmetic && enums.is_some() { return Err(invalid("enums cannot be combined with calculations")); }

    Ok(Field { name, kind, offset, explicit_offset, mask, shift, multiplier, divisor, addition,
        arithmetic, unit, precision, enums })
}

fn evaluate(field: &Field, payload: &[u8], offset: usize) -> String {
    let Some(end) = offset.checked_add(field.kind.bytes()) else { return "--".into(); };
    let Some(bytes) = payload.get(offset..end) else { return "--".into(); };
    match &field.kind {
        FieldType::Text { utf8, .. } => {
            let data = bytes.split(|byte| *byte == 0).next().unwrap_or(bytes);
            if *utf8 {
                String::from_utf8_lossy(data).to_string()
            } else {
                data.iter().map(|byte| char::from(*byte)).collect()
            }
        }
        FieldType::Float { bytes: count, little } => {
            let bits = read_integer(bytes, *little);
            let value = if *count == 4 {
                f32::from_bits(bits as u32) as f64
            } else { f64::from_bits(bits) };
            display_numeric(value * field.multiplier / field.divisor + field.addition, field.precision)
        }
        FieldType::Integer { signed, little, bytes: count } => {
            let raw = read_integer(bytes, *little);
            let mut value = field.mask.map_or(raw, |mask| raw & mask);
            if let Some((left, amount)) = field.shift {
                value = if left { value.wrapping_shl(amount) } else { value >> amount };
                if *count < 8 { value &= (1u64 << (*count * 8)) - 1; }
            }
            let signed_value = if *signed && field.mask.is_none() {
                let width = *count * 8;
                ((value << (64 - width)) as i64) >> (64 - width)
            } else { value as i64 };
            if let Some(labels) = &field.enums {
                if let Some(label) = labels.labels.get(&(signed_value as u64)) {
                    return label.clone();
                }
                if let Some(fallback) = &labels.fallback { return fallback.clone(); }
            }
            if field.arithmetic || field.precision.is_some() {
                let base = if *signed && field.mask.is_none() { signed_value as f64 } else { value as f64 };
                display_numeric(base * field.multiplier / field.divisor + field.addition, field.precision)
            } else if *signed && field.mask.is_none() {
                signed_value.to_string()
            } else { value.to_string() }
        }
        FieldType::Bit { bit, style } => {
            let value = ((bytes[0] >> *bit) & 1) as u64;
            if let Some(labels) = &field.enums {
                if let Some(label) = labels.labels.get(&value) { return label.clone(); }
                if let Some(fallback) = &labels.fallback { return fallback.clone(); }
            }
            match style {
                BitStyle::Numeric => value.to_string(),
                BitStyle::Boolean => if value == 0 { "False" } else { "True" }.to_string(),
                BitStyle::Flag => if value == 0 { "[ ]" } else { "[X]" }.to_string(),
            }
        }
    }
}

fn read_integer(data: &[u8], little: bool) -> u64 {
    if little {
        data.iter().rev().fold(0u64, |n, byte| (n << 8) | u64::from(*byte))
    } else {
        data.iter().fold(0u64, |n, byte| (n << 8) | u64::from(*byte))
    }
}

fn display_numeric(value: f64, precision: Option<usize>) -> String {
    match precision {
        Some(precision) => format!("{value:.precision$}"),
        None => value.to_string(),
    }
}

/// Exact per-interface CAN-ID rules take priority over legacy numeric-ID rules.
#[derive(Debug, Default, Clone)]
pub struct PayloadFormatRouter {
    specific: HashMap<(String, CanId), PayloadFormatter>,
    legacy_by_id: HashMap<u32, PayloadFormatter>,
}

impl PayloadFormatRouter {
    pub fn new() -> Self { Self::default() }
    pub fn assign(&mut self, interface: impl Into<String>, id: CanId, formatter: PayloadFormatter) {
        self.specific.insert((interface.into(), id), formatter);
    }
    pub fn assign_legacy_id(&mut self, id: u32, formatter: PayloadFormatter) {
        self.legacy_by_id.insert(id, formatter);
    }
    pub fn formatter_for(&self, interface: &str, id: CanId) -> Option<&PayloadFormatter> {
        self.specific.get(&(interface.to_owned(), id))
            .or_else(|| self.legacy_by_id.get(&id.value()))
    }
    pub fn format_frame(&self, frame: &CanFrame) -> Option<String> {
        if !matches!(frame.kind, FrameKind::Data) { return None; }
        self.formatter_for(&frame.interface, frame.id).map(|formatter| formatter.format(&frame.data))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn hex(raw: &str) -> Vec<u8> {
        raw.as_bytes().chunks_exact(2).map(|chunk| {
            u8::from_str_radix(std::str::from_utf8(chunk).unwrap(), 16).unwrap()
        }).collect()
    }
    fn formatted(expression: &str, data: &str) -> String {
        PayloadFormatter::compile(expression, false).unwrap().format(&hex(data))
    }
    #[test]
    fn endian_and_signed_types() {
        assert_eq!(formatted("u16le u16be i16le i16be u32le i32be", "34121234FEFFFFFE78563412FFFFFFFE"),
            "4660 4660 -2 -2 305419896 -2");
        assert_eq!(formatted("u8 i8 u16le i32le", "7FFF341200000080"), "127 -1 4660 -2147483648");
    }
    #[test]
    fn repeated_and_partial() {
        let parser = PayloadFormatter::compile("u16le", true).unwrap();
        assert_eq!(parser.format(&hex("0100FF7F00803412")), "1 32767 32768 4660");
        assert_eq!(parser.format(&hex("010002")), "1 --");
        assert_eq!(formatted("u16le u32le u8", "010203"), "513 -- --");
    }
    #[test]
    fn entire_64_bit_integer_precision() {
        assert_eq!(formatted("u64be i64le", "FFFFFFFFFFFFFFFF0000000000000080"),
            "18446744073709551615 -9223372036854775808");
    }
    #[test]
    fn arithmetic_and_names() {
        assert_eq!(formatted("rpm:u16be*0.25 temp:i8-40 voltage:u16le/10", "1C84C87405"),
            "rpm=1825 temp=-96 voltage=139.6");
        assert_eq!(formatted("gear:u16be@0&0x0700>>8 temp:i8@2*0.5-40[C]{1}", "050064"),
            "gear=5 temp=10.0 C");
    }
    #[test]
    fn text_and_out_of_bounds_warnings() {
        let formatter = PayloadFormatter::compile("vin:ascii17 label:utf88 serial:str6@30", false).unwrap();
        let mut input = b"WVWZZZ1JZXW000001".to_vec();
        input.extend_from_slice("Grüße".as_bytes());
        input.extend_from_slice(&[0u8; 32 - 17 - 7]);
        assert_eq!(formatter.format(&input), "vin=WVWZZZ1JZXW000001 label=Grüße serial=--");
        assert_eq!(formatter.warnings(input.len()),
            vec!["serial requires bytes 30-35, but payload has 32 bytes"]);
        input.truncate(30);
        input.extend_from_slice(b"ABC123");
        assert_eq!(formatter.format(&input), "vin=WVWZZZ1JZXW000001 label=Grüße serial=ABC123");
    }
    #[test]
    fn flags_and_floats() {
        assert_eq!(formatted("flags:u16be@1&0x07FF single:f32le@3 precise:f64be@7",
                "AAFFFF0000C03F4004000000000000"),
            "flags=2047 single=1.5 precise=2.5");
    }
    #[test]
    fn enum_mapping_and_fallback() {
        assert_eq!(formatted("regen:u8@0&0x01{0:Inactive,1:Active} type:u8@0&0x02>>1{0:Passive,1:Active} state:u8@1{0:Off,1:On,*:Unknown}", "0307"),
            "regen=Active type=Active state=Unknown");
        assert_eq!(formatted("state:u8{0:Off,1:On}", "02"), "state=2");
        assert_eq!(formatted("gear:i8{-1:Invalid,0:Neutral,1:First}", "FF"), "gear=Invalid");
    }
    #[test]
    fn bit_styles_and_enums() {
        assert_eq!(formatted("raw:bit0@0 enabled:bool1@0 warning:flag7@0 named:bit2@0{0:Idle,1:Active}", "86"),
            "raw=0 enabled=True warning=[X] named=Active");
    }
    #[test]
    fn rejects_invalid_expressions() {
        for expression in ["", "u24le", "u8/0", "u8*0{0:Off}", "f32le&1",
            "str2[unit]", "bool9", "bit0>>1", "u8{0:Off,0:On}",
            "u8{*:Unknown,*:Other}", "u8{0:Off}{1:On}", "u8{2}*3",
            "0invalid:u8", "u16le>>16"] {
            assert!(PayloadFormatter::compile(expression, false).is_err(), "{expression}");
        }
        assert!(PayloadFormatter::compile("u8 u8", true).is_err());
        assert!(PayloadFormatter::compile("u8@1", true).is_err());
    }
    #[test]
    fn output_fields_and_router_priority() {
        let p = PayloadFormatter::compile("rpm:u16be/4[rpm]{1}", false).unwrap();
        assert_eq!(p.format_fields(&[0x1C, 0x84])[0],
            FormattedField { name: Some("rpm".into()), value: "1825.0".into(), unit: "rpm".into() });
        let id = CanId::standard(0x123).unwrap();
        let mut router = PayloadFormatRouter::new();
        router.assign_legacy_id(0x123, PayloadFormatter::compile("u8", false).unwrap());
        router.assign("can0", id, PayloadFormatter::compile("u16be", false).unwrap());
        assert_eq!(router.formatter_for("can0", id).unwrap().format(&[0, 2]), "2");
        assert_eq!(router.formatter_for("can1", id).unwrap().format(&[2, 3]), "2");
        assert_eq!(router.formatter_for("can0", CanId::extended(0x123).unwrap()).unwrap().format(&[2, 3]), "2");
    }
}
