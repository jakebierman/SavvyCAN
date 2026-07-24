# Diagnostic and CAN Tool Feature Roadmap

This document compares SavvyCAN's workbenches with commonly available diagnostic and vehicle-network tooling. It is a capability backlog, not a claim that every listed item is implemented.

## Implemented in this branch

- OBD live PID lookup, custom PIDs, shared payload formatting, polling, multi-ECU responses, live numeric graphs with PNG export, a persistent movable/resizable widget dashboard with level, radial gauge, digital, text/string, and custom-calculation widgets, module/PID discovery, DTC reading/clearing with a versioned description-database import, readiness decoding, Mode 02 freeze-frame requests and PID enumeration, Mode 06 monitor results, Mode 09 vehicle information, trip CSV recording/playback synchronized to the CAN trace timeline, and diagnostic report export.
- UDS sessions, Tester Present, DID lists and polling, shared formatting, live numeric graphs, DID range discovery with stop/resume and saved results, guarded standard-service discovery, detailed DTC request construction, RoutineControl, guarded ECU Reset/CommunicationControl/WriteDataByIdentifier/InputOutputControl, manual SecurityAccess seed/key exchange, CSV logging, profiles, and arbitrary manual services.
- CANopen passive heartbeat/boot-up discovery, node-guard probing, heartbeat age and NMT state display, guarded NMT controls, expedited and segmented SDO uploads/downloads with abort descriptions, EDS/DCF object-dictionary import and DCF export, raw PDO monitoring, EMCY history, and SYNC/TIME monitoring and production.
- Initial local AI Analysis workbench with Ollama model discovery, selectable
  primary and reviewer models, bus/CAN-ID/recent-frame capture scoping,
  deterministic per-ID evidence, cancellable requests and an audit view. Model
  transmission remains intentionally disabled pending the native policy gate.

## OBD feature gaps

OBD Auto Doctor and OBDLink expose freeze-frame reports, Mode 06, emissions readiness, graph/image export, dashboards, diagnostic reports, custom PIDs, enhanced manufacturer diagnostics, selectable units, and large offline DTC databases.

Recommended additions:

- Package a redistributable Mode 06 Unit and Scaling ID table; readiness decoding and source/version-aware UAS imports are implemented.
- Extend freeze-frame enumeration across every bitmap page and optionally request all reported values.
- Package only redistributable DTC descriptions; the source/version-aware JSON importer is implemented.
- Add optional warning/alert thresholds, color rules, and additional gauge themes to dashboard widgets.
- Improve trip/raw-CAN synchronization with an explicit shared capture marker when diagnostic recording begins.
- Add enhanced-diagnostics definition imports rather than hard-coding manufacturer PIDs.

References: [OBD Auto Doctor features](https://www.obdautodoctor.com/features/), [OBDLink application](https://www.obdlink.com/obd-apps/obdlink-app/).

## UDS feature gaps

Vehicle Spy and CANoe-class tools use diagnostic databases to build and decode jobs, support repeatable automated sequences, and combine diagnostics with simulation, logging and testing.

Recommended additions:

- ODX/PDX import for ECU variants, services, DIDs, DTCs, routines, scaling and coded values.
- DoIP transport and vehicle announcement/routing activation.
- Reusable diagnostic jobs with variables, assertions, loops, timing and saved reports.
- A documented seed/key provider API loaded only from user-authorized modules.
- Extend service discovery across multiple sessions with retry policy, P2/P2* timing and NRC statistics.
- Full DTC status, severity, snapshot and extended-data decoding.
- Periodic DID (`0x2A`), dynamically defined DID (`0x2C`) and ResponseOnEvent workflows.
- ECU simulation and deterministic positive/negative response scenarios for bench testing.
- Flash workflow descriptions driven by ODX rather than a fixed uploader sequence.

References: [Vehicle Spy diagnostics](https://intrepidcs.com/applications/diagnostics-testing-validation/), [Vehicle Spy product capabilities](https://intrepidcs.com/products/software/vehicle-spy/), [Vector diagnostics with CAPL](https://support.vector.com/sys_attachment.do?sys_id=2163eca59750f9181245b1ece053af31&sysparm_viewer_id=4c03758d5985f9181245b1ece053af31&sysparm_viewer_table=kb_knowledge).

## General CAN-tool gaps

Current broad vehicle-network tools combine raw tracing with databases, historical buffers, statistics, error-state visibility, transmit automation, node simulation, triggers and several higher-layer protocols.

Recommended additions:

- Virtualized multi-million-frame history with re-decode after database changes.
- Multiple databases per bus, conflict detection and extended multiplexing.
- [x] Bus load, controller state, observed error-category counters and decoded error-frame analysis. Hardware TEC/REC is identified as unavailable when an adapter does not expose it.
- Triggered pre/post capture, transmit jobs, deterministic sequencing and assertions.
- Signal statistics, comparisons, anomaly detection and synchronized multi-log analysis.
- Gateway and node simulation with scripted behavior.
- LIN, Automotive Ethernet/DoIP, XCP/CCP, NMEA 2000, ISOBUS and improved J1939 workflows.
- Import/export interoperability for MDF4, BLF, ASC, PCAP/PCAPNG and decoded CSV.

References: [Vehicle Spy specifications](https://docs.intrepidcs.com/vspy-3-documentation/vehicle-spy-introduction/vehicle-spy-specifications), [webCAN capabilities](https://www.csselectronics.com/pages/webcan-can-bus-streaming-software-browser), [CSS supported protocols](https://www.csselectronics.com/).

## AI-assisted reverse engineering

The preferred design is a local-first AI Analysis workbench backed by
deterministic CAN statistics. The model should query summarized evidence rather
than receiving an unbounded raw trace, and every proposed formatter or DBC
change should remain reviewable.

Recommended phases:

- Passive analysis: selected capture/range input, bus and CAN-ID allowlists,
  bit-transition and entropy maps, periodicity, counters, checksum candidates,
  correlations, experiment comparison, and structured formatter/DBC proposals.
- Experiment sessions: label actions such as `idle`, `press brake`, or
  `increase fan`; retain before/during/after markers and let the assistant rank
  signals that explain the change.
- Explicit access selector with **Read only** as the default and **Full bus
  access** as a separately armed mode. Full access must show its active bus,
  ID allowlist, rate and payload limits, expiry time, and a persistent Stop
  control.
- Model tool calls must be proposals. A host-side policy gate validates every
  transmit request; blocked IDs and limits cannot be changed by the model.
  Diagnostic-session changes, resets, writes, security access, firmware
  operations, CANopen NMT/LSS and broadcast frames require an additional user
  confirmation regardless of the selected mode.
- Record prompts, evidence queries, tool calls, transmitted frames, responses,
  generated definitions and user approvals in an exportable audit log.
- Support local OpenAI-compatible endpoints so Ollama, llama.cpp and similar
  runtimes can be changed without coupling the workbench to one model.
- Use schema-constrained output for hypotheses, evidence, confidence, proposed
  tests and formatter/DBC patches. Never apply generated definitions silently.
- Add replay/simulation mode before live transmission, plus automatic disarm on
  disconnect, timeout, project change, model change or workbench close.

No general-purpose language model should be treated as a CAN decoder by itself.
Statistical analysis supplies the evidence; the model helps choose queries,
explain results, design controlled experiments and assemble candidate
definitions.

Deferred alternative: a future C# service or Avalonia client could use the same
versioned local API, analysis records and policy model. Rewriting the current Qt
application is not required for AI integration.

## CANopen workbench

CANopen should be a dedicated workbench built on CiA 301 concepts rather than treated as another DBC preset.

Implemented core and follow-on scope:

- [x] Passive node discovery from heartbeat/boot-up traffic and active NMT node guarding where appropriate.
- [x] Node list showing NMT state, heartbeat age, identity and last emergency.
- [x] NMT Start, Stop, Pre-operational and Reset commands with confirmation for reset operations.
- [x] Expedited, segmented and block SDO upload/download with abort-code descriptions.
- [x] Object Dictionary browser with index/sub-index, access type, data type and typed value formatting.
- [x] EDS/DCF import and export, including compact object descriptions and defaults.
- [x] PDO monitor with EDS/DCF mapping decode and live graphing.
- [x] Guarded PDO remapping workflow.
- [x] EMCY history with error register and manufacturer data.
- [x] SYNC and TIME production/monitoring.
- [x] Guarded LSS discovery and node-ID/bit-rate configuration.
- [x] CiA 402 drive-state decoding and confirmed standard controlword commands.

CiA identifies SDO as the mechanism for object-dictionary access; the CiA 301 communication model also covers NMT, PDO, EMCY, heartbeat, SYNC and standard communication-profile objects. References: [CiA SDO protocol](https://can-cia.org/can-knowledge/sdo-protocol), [CiA 301 poster](https://member.can-cia.org/files/canopen_poster_2024.pdf), [CANopen profiles](https://www.can-cia.org/can-knowledge/canopen-profiles).
