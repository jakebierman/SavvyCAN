# SavvyCAN Rust migration (in progress)

This directory contains the **new Rust implementation**. The original Qt/C++
SavvyCAN remains in the repository root for feature parity and regression
testing. Nothing here replaces the established application or its release
pipeline yet.

## Why this layout?

- **savvycan-core**: portable pure-Rust CAN frames, validation, candump import,
  DBC loading and decoding, typed payload formatting, and summary metrics. Shared by Slint, CLI tools,
  tests and future adapters. No Qt, Slint or operating-system-specific code.
- **savvycan-desktop**: Slint graphical shell. Currently **offline and read-only**.
  Open a supported CAN trace, optionally load a DBC and a typed format, then browse raw and decoded
  frames. Input errors include source line numbers.

## Run

Install a current stable Rust toolchain and Linux GUI development dependencies.

From this directory:

~~~sh
cargo test -p savvycan-core
cargo run -p savvycan-desktop -- /path/to/candump.log /path/to/signals.dbc "rpm:u16be@0/4[rpm]{1}"
~~~

The DBC path, typed format and rules-file path are optional. To pass a format without a DBC, use
an empty string for the DBC argument. You can also enter log/DBC paths and the
formatter expression directly in the UI. Optionally specify a per-ID rules file
as the fourth positional argument or in the viewer. Per-ID rules override the
default typed expression, and RTR requests are never formatted.

## Current feature boundary

| Feature | Rust implementation |
| --- | --- |
| Validated 11/29-bit identifiers, classic CAN and CAN FD payloads | Done |
| Per-interface identity, microsecond timestamps, FD flags and RTR | Done |
| Linux candump compact and simple bracketed-text log import | Done |
| CRTD, GVRET native CSV (V1/V2), generic ID/data CSV imports | Done (content autodetection and source-line errors) |
| BUSMASTER and Microchip Analyzer classic CAN logs | Done (including BUSMASTER RTR and Tx/Rx direction) |
| CRTD and GVRET native CSV V2 exports | Done for representable classic frames; UI creates new files only |
| Offline trace statistics and first 25,000 rows in a Slint viewer | Done |
| DBC message/signal load, signed/unsigned Intel/Motorola, scale/offset | Initial support |
| DBC enumerations, IEEE 32/64-bit float, simple and nested mux ranges | Initial support |
| DBC matching for J1939/GMLAN PGN profiles, multi-file precedence | Not yet |
| BLF, PCAP, CANalyzer, PCAN and other source formats | Not yet |
| Custom typed payload formatter and per-ID rules (separate from DBC) | Initial support: portable parser/router, Slint global and rules-file previews |
| SocketCAN, GVRET serial, SLCAN, QtSerialBus equivalents, MQTT | Not yet |
| Playback, transmit, scripting, OBD, UDS, CANopen, graphing, AI | Not yet |

Capture import autodetects candump, CRTD, GVRET/generic CSV, BUSMASTER and
Microchip logs. The viewer also exports CRTD and GVRET CSV V2 to a user-selected **new** file;
existing paths are never overwritten. Those writers deliberately reject frames they cannot
round-trip (CAN FD, RTR, missing timestamps or unknown Rx/Tx direction) rather
than truncate or invent data. Generic ID/data CSV has no timestamps or direction
and retains those values as unknown.

The pure-Rust typed formatter supports integer/float byte order, signed values,
text, individual bits, masks and shifts, arithmetic, units, fixed precision,
enum labels, missing-byte markers, validation warnings and repeated fields. A
format is compiled once before opening a trace. Invalid formats fail with a
user-visible error. Per-interface/ID-specific and legacy numeric ID fallback routing are supported
through a text file loaded by the Slint viewer. An interactive per-ID rule
editor is still pending.
Refer to ../help/payload_formatter.md for grammar details.

DBC parsing covers BO_, SG_, VAL_, SIG_VALTYPE_, and SG_MUL_VAL_ directives.
Other metadata is not imported yet. A malformed supported directive or
out-of-range signal is reported rather than silently producing a value.
Extended ID flags use the standard DBC bit-31 convention. Decoding only applies
to data frames, never RTR frames. Capture interface and DBC association is still
global (a single optional DBC), not SavvyCAN's configurable per-bus file
ordering. Do not assume full DBC compatibility yet.

**Safety:** the Rust GUI has no transmit or device-control path. When those are
ported, preserve SavvyCAN's read-only default, access/arming/confirmation policy,
and explicit adapter capability validation.

## Migration order

1. Shared frame/trace types, import fixtures and tests (started).
2. File import/export parity and streaming trace data/filtering.
3. DBC decoding plus the existing typed payload formatter's semantics (started).
4. Adapter trait and transports, beginning with SocketCAN and GVRET,
   preserving CAN FD capabilities and bus-specific settings.
5. Core workbenches and guarded transmit/playback operations.
6. Slint interaction parity, scripting/AI gates, cross-platform packaging.

Build/test workflows are separate from the existing Qt release process.
Do not merge or replace C++ binaries until parity is measured and verified.

## Typed formatter rules file

Each nonempty line assigns a payload expression to an interface and CAN ID.
IDs are hexadecimal; `S:` forces 11-bit, `X:` forces 29-bit, and an
unqualified identifier chooses extended only above 0x7FF. Use `*` for a
numeric-ID fallback when no interface-specific rule exists.

~~~text
# interface CAN-ID custom-format
can0 S:123 rpm:u16be@0/4[rpm]{1} coolant:i8@2-40[C]
can0 X:00000123 status:u8{0:Off,1:On}
* 0x456 voltage:u16le/1000[V]{3}
~~~

The viewer applies matching rules first, then the optional global formatter.
Names like `can0` and `bus0` must match the imported capture's interface
exactly. Neither DBC decoding nor the underlying frame bytes are changed.

## Export limitations

Select an output path ending in `.crtd`, `.crt`, or `.csv` and click
**Export trace**. Export is available only for classic CAN data frames with
known microsecond timestamps, bus numbers and Rx/Tx directions. It rejects
CAN FD, RTR and missing metadata before creating the output file; an existing
file is never replaced. The current exporters are intentionally conservative
because the legacy formats cannot represent every field.
