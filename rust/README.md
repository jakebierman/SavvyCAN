# SavvyCAN Rust migration (in progress)

This directory contains the **new Rust implementation**. The original Qt/C++
SavvyCAN remains in the repository root for feature parity and regression
testing. Nothing here replaces the established application or its release
pipeline yet.

## Why this layout?

- **savvycan-core**: portable pure-Rust CAN frames, validation, candump import,
  DBC loading and decoding, and summary metrics. Shared by Slint, CLI tools,
  tests and future adapters. No Qt, Slint or operating-system-specific code.
- **savvycan-desktop**: Slint graphical shell. Currently **offline and read-only**.
  Open a Linux candump log, optionally load a DBC, and browse raw and decoded
  frames. Input errors include source line numbers.

## Run

Install a current stable Rust toolchain and Linux GUI development dependencies.

From this directory:

~~~sh
cargo test -p savvycan-core
cargo run -p savvycan-desktop -- /path/to/candump.log /path/to/signals.dbc
~~~

The second argument is optional. You can also run the desktop without arguments
and enter the log and optional DBC paths in the UI.

## Current feature boundary

| Feature | Rust implementation |
| --- | --- |
| Validated 11/29-bit identifiers, classic CAN and CAN FD payloads | Done |
| Per-interface identity, microsecond timestamps, FD flags and RTR | Done |
| Linux candump compact and simple bracketed-text log import | Done |
| Offline trace statistics and first 25,000 rows in a Slint viewer | Done |
| DBC message/signal load, signed/unsigned Intel/Motorola, scale/offset | Initial support |
| DBC enumerations, IEEE 32/64-bit float, simple and nested mux ranges | Initial support |
| DBC matching for J1939/GMLAN PGN profiles, multi-file precedence | Not yet |
| GVRET, CRTD, BLF, PCAP, CSV and other source formats | Not yet |
| Custom typed payload formatter and per-ID rules (separate from DBC) | Not yet |
| SocketCAN, GVRET serial, SLCAN, QtSerialBus equivalents, MQTT | Not yet |
| Playback, transmit, scripting, OBD, UDS, CANopen, graphing, AI | Not yet |

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
