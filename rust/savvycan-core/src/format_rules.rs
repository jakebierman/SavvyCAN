//! Load per-interface and legacy-ID payload formatter assignments.
//! One rule per line: <interface|*> <S|X:id> <payload format expression>
//! IDs are hexadecimal. "*" is a legacy numeric-ID fallback that ignores frame width.
use crate::{CanId, FormatError, PayloadFormatRouter, PayloadFormatter};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuleParseError {
    pub line: usize,
    pub message: String,
}

impl fmt::Display for RuleParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "format rules line {}: {}", self.line, self.message)
    }
}
impl std::error::Error for RuleParseError {}

fn parse_id(input: &str) -> Result<(CanId, u32), String> {
    let (width, raw) = if let Some(raw) = input.strip_prefix("S:").or_else(|| input.strip_prefix("s:")) {
        (Some(false), raw)
    } else if let Some(raw) = input.strip_prefix("X:").or_else(|| input.strip_prefix("x:")) {
        (Some(true), raw)
    } else {
        (None, input)
    };
    let raw = raw.strip_prefix("0x").or_else(|| raw.strip_prefix("0X")).unwrap_or(raw);
    let value = u32::from_str_radix(raw, 16).map_err(|_| format!("invalid hexadecimal CAN ID '{input}'"))?;
    let extended = width.unwrap_or(value > 0x7ff);
    let id = if extended {
        CanId::extended(value).map_err(|err| err.to_string())?
    } else {
        if value > CanId::MAX_STANDARD as u32 { return Err("standard CAN ID exceeds 7FF".into()); }
        CanId::standard(value as u16).map_err(|err| err.to_string())?
    };
    Ok((id, value))
}

fn rule(line: &str, router: &mut PayloadFormatRouter) -> Result<(), String> {
    let mut words = line.split_whitespace();
    let scope = words.next().ok_or("missing interface")?;
    let id_str = words.next().ok_or("missing CAN identifier")?;
    // Keep the complete formatter (including whitespace between fields).
    let expression = line[scope.len()..].trim_start()[id_str.len()..].trim();
    if expression.is_empty() { return Err("missing formatter expression".into()); }
    let (id, value) = parse_id(id_str)?;
    let formatter = PayloadFormatter::compile(expression, false)
        .map_err(|FormatError(reason)| reason)?;
    if scope == "*" {
        router.assign_legacy_id(value, formatter);
    } else {
        if !scope.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.')) {
            return Err("interface contains invalid characters".into());
        }
        router.assign(scope, id, formatter);
    }
    Ok(())
}

/// Parse format-rule text. Reject malformed entries without returning a partial router.
pub fn parse_format_rules(contents: &str) -> Result<PayloadFormatRouter, RuleParseError> {
    let mut router = PayloadFormatRouter::new();
    for (idx, raw_line) in contents.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') { continue; }
        rule(line, &mut router).map_err(|message| RuleParseError { line: idx + 1, message })?;
    }
    Ok(router)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn per_interface_width_and_fallback_rules() {
        let router = parse_format_rules(
            "# Custom formatter assignments\n\
             * 0x123 status:u8{0:Off,1:On}\n\
             can0 S:123 rpm:u16be/4[rpm]{1}\n\
             can0 X:00000123 raw:u8\n\
             can1 S:123 scaled:u8*2\n"
        ).unwrap();
        assert_eq!(router.formatter_for("can0", CanId::Standard(0x123)).unwrap().format(&[0,4]), "rpm=1.0 rpm");
        assert_eq!(router.formatter_for("can0", CanId::Extended(0x123)).unwrap().format(&[1]), "raw=1");
        assert_eq!(router.formatter_for("can1", CanId::Standard(0x123)).unwrap().format(&[3]), "scaled=6");
        assert_eq!(router.formatter_for("can2", CanId::Standard(0x123)).unwrap().format(&[1]), "status=On");
        assert_eq!(router.formatter_for("can2", CanId::Extended(0x123)).unwrap().format(&[1]), "status=On");
    }
    #[test]
    fn malformed_rule_does_not_silently_apply() {
        for input in ["can0 S:ABC", "can0 S:ABC u16be/0", "can0 X:20000000 u8",
            "can0 S:FFF u8", "bad/interface 123 u8"] {
            assert!(parse_format_rules(input).is_err(), "{input}");
        }
        let err = parse_format_rules("# header\ncan0 S:123 u8\ncan0 S:123 u8/0")
            .unwrap_err();
        assert_eq!(err.line, 3);
    }
}
