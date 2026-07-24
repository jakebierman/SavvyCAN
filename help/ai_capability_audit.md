# AI Capability Coverage Audit

The AI action registry is the authority for what chat can inspect or change.
GUI actions are preferred; scripting is only offered when explicitly requested.

## Implemented

- Workspace navigation for the primary trace and reverse-engineering tools.
- Main trace overwrite, DBC interpretation, raw payload, ID filters and payload
  formatting.
- UDS DID lists and guarded UDS execution.
- OBD PID lists, list clearing, dashboards and guarded OBD execution.
- CANopen Object Dictionary entries, uploads, scanning and guarded writes.
- Frame Sender drafts, direct single-shot sends and bounded repeated rows.
- Fuzzing configuration and bounded guarded start.
- Graph creation, script drafts when requested, and dashboard PID widgets.
- Connection profile context, reconnect, suspend, resume and guarded profile
  creation for supported SavvyCAN connection types.

## Important Gaps

- Capture start, pause, resume, clear and time-window selection in the main
  trace.
- CAN log open, append, save, export and recent-file selection.
- DBC open, close, save, node/message/signal editing and assignment.
- Connection removal, reordering, bus enable/listen-only/single-wire settings
  and bitrate changes on existing profiles.
- Frame Sender row editing, disable/remove, batch enable and saved-grid
  loading.
- Filter clearing, ranges/masks, named filter profiles and advanced filters.
- Playback file selection, speed, loop, seek and transport controls.
- Sniffer start/stop, notch reset, row selection and export.
- Graph editing/removal, presets and export.
- UDS sessions, security providers, DTC clearing, firmware transfer fields and
  routine-control editing beyond the current prepared operations.
- OBD request editing/removal, DTC workflows, freeze frames, readiness and
  dashboard layout management.
- CANopen NMT, SDO download details, PDO mapping, EMCY history, SYNC/TIME, LSS
  and EDS/DCF file operations.
- ISO-TP configuration, bridge routes, comparator inputs and most secondary
  reverse-engineering windows.
- Preferences, dock/layout control, screenshots, clipboard operations and help
  navigation.

## Safety Boundaries

Actions which can transmit or activate hardware require the configured full
access policy. File replacement, connection removal, DBC mutation, firmware
transfer, fuzzing and high-volume sends need explicit validation and suitable
confirmation classifications before being exposed.

“Add driver” means adding a SavvyCAN connection profile using an already
available backend. Installing kernel modules, packages or arbitrary executable
drivers is outside the in-app action registry.
