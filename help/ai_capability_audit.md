# AI Capability Coverage Audit

The AI action registry is the authority for what chat can inspect or change.
GUI actions are preferred; scripting is only offered when explicitly requested.

## Implemented

- Workspace navigation for the primary trace, diagnostic, analysis and
  reverse-engineering tools.
- Main trace overwrite, DBC interpretation, raw payload, ID filters and payload
  formatting.
- UDS DID list add/clear, endpoint connect/disconnect, one-shot and polling
  requests, ECU/DID/service/session discovery, Tester Present, DTC reads and
  guarded DTC clearing, RoutineControl, SecurityAccess seed/key, detailed DTC
  requests, and guarded arbitrary service/control requests.
- OBD PID lists, endpoint control, one-shot and polling requests, module/PID
  discovery, stored/pending/permanent DTC reads, guarded clearing, Mode 09
  vehicle information, freeze frames, Mode 06 monitors, readiness, individual
  request editing/removal, and complete dashboard layout load/save/replace.
- CANopen Object Dictionary entries, SDO uploads and guarded writes, node
  scanning, NMT, SYNC, TIME, LSS discovery/configuration, EDS/DCF import/export,
  EMCY clearing and CiA 402 state reads.
- Compact CAN Trace sender context, draft creation/edit/removal, structured
  byte/bit updates, start/stop, selected-row one-shot sends, JSON load/save, selected-frame copy and
  advanced-sender transfer. The advanced Frame Sender also supports draft
  editing/removal, batch enable/disable, `.fsd` grids, triggers and modifiers.
- Filter ID/range/mask editing, clear, and filter profile load/save.
- Playback file/live loading, timing/burst/bus options, sequence removal and
  guarded forward/reverse/step transport.
- Sniffer clear/pause/notch controls plus differential experiment capture,
  counter/checksum inference, diagnostic correlation and signal clustering.
- DBC load/create/save/close, bus assignment, and guarded node/message/signal
  editing with reference checks.
- Fuzzing configuration and bounded guarded start.
- Graph creation, script drafts when requested, and dashboard PID widgets.
- Connection profile context, reconnect, suspend, resume and guarded profile
  creation for supported SavvyCAN connection types.

## Important Gaps

- Capture start, pause, resume, clear and time-window selection in the main
  trace.
- CAN log open, append, save, export and recent-file selection.
- Connection removal, reordering, bus enable/listen-only/single-wire settings
  and bitrate changes on existing profiles.
- Main-trace named advanced-filter expressions beyond the current formatter and
  ID/range/mask profile controls.
- Playback arbitrary seek and per-sequence filter file automation.
- Sniffer analysis export to an explicit path and individual row selection.
- Graph editing/removal, presets and export.
- UDS external security-provider configuration and firmware-transfer field
  automation.
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
