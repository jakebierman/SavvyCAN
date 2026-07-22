# Payload Formatter Reference

SavvyCAN uses one shared payload formatter in the main frame list, per-frame assignments, UDS DID requests, and OBD-II custom PID rows. A format describes how bytes in a payload become named, calculated, and labelled values.

## Field grammar

A format contains one or more whitespace-separated fields:

```text
[name:]type[@offset][&mask][>>shift][*multiplier][/divisor][+offset|-offset][unit]{precision}{enum}
```

Only `type` is required. Modifiers must appear in the order shown. Examples:

```text
u8
rpm:u16be@0/4[rpm]{1}
temperature:i8@2-40[C]{1}
gear:u16be@3&0x0700>>8
regen:u8@5&0x01{0:Inactive,1:Active}
vin:ascii17
```

Field names contain letters, digits, and underscores and must start with a letter or underscore. Output without a field name contains only the value. Named output uses `name=value`.

## Numeric types

| Type | Meaning | Bytes |
| --- | --- | ---: |
| `u8`, `i8` | Unsigned or signed 8-bit integer | 1 |
| `u16le`, `i16le` | Unsigned or signed 16-bit little-endian integer | 2 |
| `u16be`, `i16be` | Unsigned or signed 16-bit big-endian integer | 2 |
| `u32le`, `i32le`, `u32be`, `i32be` | 32-bit integer | 4 |
| `u64le`, `i64le`, `u64be`, `i64be` | 64-bit integer | 8 |
| `f32le`, `f32be` | IEEE 754 single-precision value | 4 |
| `f64le`, `f64be` | IEEE 754 double-precision value | 8 |

`le` means the least-significant byte is first. `be` means the most-significant byte is first. Signed integers use two's-complement interpretation. Floating-point fields support calculations, units, and precision, but not masks or enums.

## Text types

`asciiN`, `utf8N`, and `strN` read exactly `N` bytes. `ascii` decodes Latin-1 text; `utf8` and `str` decode UTF-8. Reading stops at the first null byte.

```text
vin:ascii17
label:utf88
serial:str6@20
```

Text fields support names and offsets but do not support masks, shifts, calculations, units, precision, or enums.

## Byte positions

Offsets are zero-based. `@0` reads from the first payload byte and `@3` reads from the fourth. A field without an explicit offset begins immediately after the preceding field:

```text
speed:u16be temperature:i8 flags:u8
```

The fields above begin at offsets 0, 2, and 3. After an explicitly positioned field, the next implicit field begins after that field. Explicit offsets may overlap, which is useful when extracting several bit fields from one byte:

```text
regen:u8@0&1 type:u8@0&2>>1
```

If a field extends beyond the available payload, its value is `--`. The main-window preview also reports the required byte range.

## Masks and shifts

`&mask` applies an unsigned bit mask. Masks may be decimal or hexadecimal:

```text
flags:u16be&0x07FF
```

`>>N` shifts right and `<<N` shifts left after masking. The shift must be smaller than the field width:

```text
gear:u16be&0x0700>>8
```

A masked signed field is treated as an unsigned bit field. Use a separate signed field when the extracted result needs two's-complement interpretation.

## Calculations

Calculations are applied in this order:

```text
result = value * multiplier / divisor + additive_offset
```

Each component is optional, but its position is fixed. Decimal and negative factors are accepted. Division by zero is rejected.

```text
rpm:u16be/4[rpm]{1}
pressure:u16be*0.079[kPa]{3}
temperature:u8-40[C]
trim:u8*100/128-100[%]{1}
```

The formatter performs calculations using floating-point arithmetic. For signed fields, sign conversion occurs before the calculation unless a mask was applied.

## Units and precision

`[unit]` appends a unit to displayed output and exposes it separately to decoded CSV columns. `{precision}` fixes the number of digits after the decimal point:

```text
voltage:u16be/1000[V]{3}
```

Without a precision modifier, trailing insignificant zeroes are omitted. Precision counts decimal places, not significant figures.

## Enums and statuses

An enum maps the integer value remaining after mask and shift operations to text:

```text
state:u8{0:Off,1:On}
regen:u8&0x01{0:Inactive,1:Active}
type:u8&0x02>>1{0:Passive,1:Active}
gear:i8{-1:Invalid,0:Neutral,1:First}
```

Decimal, hexadecimal, and negative keys are accepted. Add `*` to define an unknown-value fallback:

```text
state:u8{0:Off,1:On,*:Unknown}
```

Without `*`, an unmapped value remains numeric. Enum labels cannot contain whitespace because whitespace separates fields. Commas, colons, and braces are reserved by the enum syntax. Enums cannot be combined with calculations or floating-point fields.

## Evaluation order

For each numeric field, SavvyCAN performs these operations:

1. Read the requested bytes using the selected byte order.
2. Apply the mask, when present.
3. Apply the right or left shift.
4. Resolve an enum label, when present.
5. Otherwise apply the numeric calculation.
6. Apply display precision and append the unit.

## Repeated fields

Typed main-window presets repeat a single field across the complete payload. Some workbench inputs also repeat a lone simple type, such as `u16be`, across returned data. Multi-field formats and explicitly positioned fields describe a fixed layout instead.

If the final repeated value is incomplete, it is displayed as `--` rather than reading beyond the payload.

## Complete examples

An engine response containing RPM, temperature, and two status bits:

```text
rpm:u16be@0/4[rpm]{0} temp:u8@2-40[C]{1} regen:u8@3&1{0:Inactive,1:Active} regen_type:u8@3&2>>1{0:Passive,1:Active}
```

Several values extracted from the same flags byte:

```text
ready:u8@0&1{0:No,1:Yes} fault:u8@0&2>>1{0:Clear,1:Set} mode:u8@0&0x0C>>2{0:Off,1:Idle,2:Run,*:Reserved}
```

A mixed fixed-layout identification response:

```text
version:u16be@0 serial:ascii12@2 status:u8@14{0:Invalid,1:Valid,*:Unknown}
```

## Automatic protocol decoding

`auto` is an OBD-II Workbench setting, not formatter syntax. It selects a built-in decoder or standard PID calculation. Structured PIDs may inspect availability bits and emit a variable number of values, which a fixed custom format cannot conditionally hide. If no built-in decoder exists, `auto` shows raw hexadecimal bytes.

Entering a custom format in an OBD row overrides `auto`. UDS DIDs have no universal standard layout, so their formats are always chosen by the user or loaded from a saved DID list.

DBC interpretation is separate from this grammar. The main window can generate a compatible custom format from byte-aligned DBC signals; signals whose bit layout cannot be represented safely remain handled by the DBC interpreter.

## Validation

Compilation rejects unknown types, misplaced modifiers, zero divisors, masks on floating-point values, shifts outside the field width, duplicate enum values, duplicate fallbacks, invalid enum entries, and unsupported modifiers on strings or enums. A previously active format is not replaced when a new expression fails validation.
