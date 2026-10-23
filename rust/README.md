# SavvyCAN Rust migration (in progress)

This directory contains the **new Rust implementation**. The original Qt/C++
SavvyCAN remains in the repository root for feature parity and regression
testing. Nothing here replaces the established application or its release
pipeline yet.

## Why this layout?

- **savvycan-core**: portable pure-Rust CAN frames, validation, candump import
  and summary metrics. Shared by Slint, CLI tools, tests and future adapters.
  No Qt, Slint or operating-system-specific code.
- **savvycan-desktop**: Slint graphical shell. Currently **offline and read-only**.
  Open a Linux candump log and browse parsed frames. Input errors include
  source line numbers.

## Run

Install a current stable Rust toolchain (Slint 1.18.1 needs a recent compiler).
On Linux, install distribution-specific GUI development dependencies as needed.

From this directory:

~~~sh
cargo test -p savvycan-core
cargo run -p savvycan-desktop -- /path/to/candump.log
~~~

You can also run the desktop without arguments and enter a file path in the UI.

## Current feature boundary

| Feature | Rust implementation |
| --- | --- |
| Validated 11/29-bit identifiers, classic CAN and CAN FD payloads | Done |
| Per-interface identity, optional microsecond timestamps, FD flags and RTR | Done |
| Linux candump compact and simple bracketed-text log import | Done |
| Offline trace statistics and first 25,000 rows in a Slint viewer | Done |
| GVRET, CRTD, BLF, PCAP, CSV and other source formats | Not yet |
| DBC loading and signal interpretation | Not yet |
| Typed payload formatting and per-ID formatting rules | Not yet |
| SocketCAN, GVRET serial, SLCAN, QtSerialBus equivalents, MQTT | Not yet |
| Playback, transmit, scripting, OBD, UDS, CANopen, graphing, AI | Not yet |

**Safety:** the Rust GUI has no transmit or device-control path. When those are
ported, preserve SavvyCAN's read-only default, access/arming/confirmation policy
and explicit adapter capability validation.

## Migration order

1. Shared frame/trace types, import fixtures and tests (started here).
2. File import/export parity and streaming trace data/filtering.
3. DBC decoding plus the existing typed payload formatter's semantics.
4. Adapter trait and transports, beginning with SocketCAN and GVRET,
   preserving CAN FD capabilities and bus-specific settings.
5. Core workbenches and guarded transmit/playback operations.
6. Slint interaction parity, scripting/AI gates, cross-platform packaging.

Build/test workflows are separate from the existing Qt release process.
Do not merge or replace C++ binaries until parity is measured and verified.
