# Qt → Rust/Slint migration parity tracker

**Canonical tracking file.** Update this file in the same commit as each implemented feature. Compare against Qt `master` and Rust `feature/rust-slint-migration`. This is an initial source/documentation-based inventory, **not** a fully exercised UI-by-UI audit. GitHub Issues are disabled (HTTP 410), so the repository file is the durable tracker.

## Workflow

1. Before implementing: fetch the current branch HEAD and this file; choose highest-priority open item, and inspect Qt implementation plus Rust equivalents.
2. During implementation: include focused regression/fixture tests, preserve receive-only default and original Qt sources.
3. After implementation: change status only for verified functionality; record commit SHA, CI run and remaining gaps in this file; update `rust/README.md`.
4. Do not mark **Done** on partial parity. Use **Partial** until all acceptance criteria pass, and keep subfeatures explicit.
5. Always use fast-forward commits with expected HEAD checks; scheduled runs and interactive chats may work concurrently.
6. Never rewrite the immutable preview tag `rust-slint-preview-2026-10-23`.

## Feature backlog

| ID | Priority | Feature | Rust status | Qt reference | Rust reference / acceptance |
|---|---|---|---|---|---|
| PAR-001 | P0 | DBC create/edit/delete, nodes/messages/signals, save/load without metadata loss | Partial: browse/search/decode only | `dbc/dbcmaineditor.cpp`, `dbc/dbcmessageeditor.cpp`, `dbc/dbcsignaleditor.cpp`, `help/dbc_editor.md` | `rust/savvycan-core/src/dbc.rs`, `dbc_stack.rs`; editable UI, round-trip unknown directives, mux and enums |
| PAR-002 | P1 | Advanced multi-series graphing, configuration save/load, live follow, axis controls and exports | Partial: up to 3 series, offline zoom/pan/cursor | `re/graphingwindow.cpp`, `help/graphwindow.md` | `rust/savvycan-core/src/timeseries.rs`, Slint UI; remove cap, persist definitions, real-time graphs |
| PAR-003 | P1 | Frame details, flow view and synchronized inspection | Missing/partial trace browser | `re/frameinfowindow.cpp`, `re/flowviewwindow.cpp` | Reproduce Qt flow/frame workflows with testable view models |
| PAR-004 | P1 | Sniffer, range/state, temporal and bisector tools | Missing | `re/sniffer/snifferwindow.cpp`, `re/rangestatewindow.cpp`, `bisectwindow.cpp` | Port read-only analysis algorithms and Slint workflows |
| PAR-005 | P1 | Capture/DBC comparison workflows | Missing | `re/filecomparatorwindow.cpp`, `re/dbccomparatorwindow.cpp` | Diff analysis and UI with fixture tests |
| PAR-006 | P1 | Adapter discovery, connection settings, GVRET handshake/TCP, socketcand, MQTT, serialbus | Partial: receive-only SocketCAN, SLCAN/GVRET serial | `connections/`, `help/connectionwindow.md` | Transport discovery, settings and hardware-tested read-only paths |
| PAR-007 | P1 | Additional trace formats and lossless import/export | Partial: many text formats, PCAP/PCAPNG; BLF missing | `framefileio.cpp`, `blfhandler.cpp` | Qt fixture comparison, BLF and remaining supported formats |
| PAR-008 | P1 | Bus diagnostics and deeper passive ISO-TP/J1939 | Partial: passive ISO-TP and J1939 BAM | `re/busdiagnosticswindow.cpp`, `bus_protocols/` | Read-only bus health, sessions, reassembly and diagnostics |
| PAR-009 | P1 | Preferences, persistent workspace state, Linux/Windows distribution | Partial: preview builds and dialogs | `mainsettingsdialog.cpp`, `mainwindow.cpp` | Save settings, test packaging, validate desktop parity |
| PAR-010 | P2 | OBD-II workbench, dashboards, custom PIDs, reports and trips | Missing | `re/obd2workbenchwindow.cpp`, `re/obddashboardcanvas.cpp` | Passive views first; active polling requires separate approval |
| PAR-011 | P2 | UDS workbench, DID/DTC/session workflows | Missing | `bus_protocols/uds_handler.cpp`, `help/diagnostic_feature_roadmap.md` | Passive analysis first; active controls excluded by default |
| PAR-012 | P2 | CANopen heartbeat/PDO/EMCY, EDS/DCF and SDO/NMT workbench | Missing | `re/canopenworkbenchwindow.cpp` | Passive read-only monitoring first |
| PAR-013 | P2 | Local AI analysis, chat, action policy and scripting | Missing | `re/aiworkbenchwindow.cpp`, `re/aiactionregistry.cpp`, `jsedit.cpp` | Local model integration and strict permission model |
| PAR-014 | P2 / safety-gated | Hardware transmit, sender, bridge, playback-to-bus, fuzzing | Deferred intentionally | `framesenderwindow.cpp`, `canbridgewindow.cpp`, `re/fuzzingwindow.cpp` | Explicit separate safety decision; no transmit API in current migration |
| PAR-015 | P1 | Qt workflow parity audit and regression corpus | Open | `help/`, Qt code and fixtures | Document each UI operation, fixtures, edge cases and parity evidence |

## Definition of Done for each item

- [ ] Concrete Qt workflow and source references checked, including edge cases.
- [ ] Rust implementation and Slint user flow complete for the agreed scope.
- [ ] Automated unit/integration/fixture tests added and passing.
- [ ] Linux, Windows and core CI verified for the implementation commit.
- [ ] `rust/README.md` feature matrix and this tracker updated with evidence.
- [ ] No regression in receive-only safety, original Qt branch or MIT attribution.

## Current checkpoint

- Baseline Qt branch: `master` at `56dcb0d7994e8e2e78f92ceb0badad41273328b7`.
- Rust branch inspected: `feature/rust-slint-migration` at `84f1832b2e3abad6b3b8e5cfc69d0a8688c3a4c6`.
- Next implementation target: **PAR-001**, beginning with DBC document preservation and validated edit/save round trips.
- Statuses are preliminary until each Qt workflow is exercised and verified.
