# UDS Workbench

Open **Send Frames > UDS Workbench** to make interactive diagnostic requests through SavvyCAN's existing ISO-TP and UDS support.

Set the CAN bus and request/response IDs, choose a diagnostic session, then select **Connect session**. Tester Present can be enabled to keep that session alive.

## DID requests

Add DIDs to the table and enter each 16-bit identifier. The **Poll** checkbox includes a row in enabled operations. Use **Send selected once** for highlighted rows, **Send enabled once** for one pass through checked rows, and the single **Start polling** / **Stop polling** button for repeated cycles. The list, endpoint, and selected session are restored the next time the workbench opens.

Select the cycle interval before starting polling. DIDs are sent sequentially, advancing immediately after each complete response or the P2 timeout. **Cycle interval** is measured from the start of one list pass to the start of the next; if a pass takes longer than the interval, the next pass starts as soon as the current one finishes. A row's **Poll ms** value is an optional minimum time between requests for that DID, so slower-changing DIDs can skip cycles; use `0` to include the row in every cycle. Only one diagnostic exchange is in flight at a time.

Use **Save DID list** to save the endpoint, session, and DID definitions as JSON. **Load DID list** replaces the current list and endpoint; reconnect the imported endpoint before sending requests.

The live graph plots every numeric field produced by the shared formatter. CSV logging records timestamped raw and decoded values for later analysis.

The Payload format column uses the same formatter as the main frame list. Simple formats such as `u8`, `i16be`, and `f32le` repeat across the returned data. Named or calculated layouts can also be used, for example `rpm:u16be*0.25`. Status values can use enums such as `regen:u8&1{0:Inactive,1:Active}` or add `*:Unknown` as a fallback. Fixed-length strings use `asciiN`, `utf8N`, or `strN`, such as `vin:ascii17`; string fields may also use a byte offset such as `serial:ascii8@20`.

See [Payload Formatter Reference](./payload_formatter.md) for the complete grammar and validation rules.

## DID discovery

The **DID discovery** tab scans a configurable inclusive range with ReadDataByIdentifier (`0x22`). The default `0xF180` through `0xF1FF` range covers common standardized identification DIDs. Set a timeout appropriate for the ECU and transport; a short timeout makes unsupported DIDs pass quickly, while a slower ECU may require more time. Discovery defaults to 300 ms and remembers the chosen timeout. If a DID works as a normal request but is absent from a scan, increase this timeout toward the endpoint's P2 value.

Only positive responses containing the requested DID are added to the result list. Negative responses and timeouts advance to the next DID, while Response Pending continues waiting for the current DID. The scan can be stopped at any time. Polling pauses during discovery and resumes afterward when enabled.

Select one or more results and choose **Add selected to DID requests**. Existing DIDs are not duplicated. Discovered DIDs use a raw `u8` format by default; VIN DID `0xF190` uses `ascii17`. Review or replace the format after adding a DID because most DID payload definitions are manufacturer-specific.

Large scans require confirmation. DID discovery sends active diagnostic requests and should only be used on modules you own or are authorised to test.

Stopped scans can be resumed in the same session. Existing positive results are retained and the discovery summary is rebuilt from the combined result list when the resumed scan stops or completes. **Save results** stores the range, next DID, and positive responses as JSON; loading that file restores the exact resume point.

## Service discovery

The **Service discovery** tab checks standard UDS service identifiers using intentionally incomplete requests. A positive response, or a negative response other than `0x11` ServiceNotSupported, indicates that the ECU recognizes that service in the current session. Timeouts and ServiceNotSupported responses are omitted.

The scan requires confirmation. It does not supply valid control, reset, write, transfer, or security parameters, but it still creates active diagnostic traffic and cannot guarantee how a non-conforming ECU will react.

## DTCs and controls

The DTC tab supports the normal status-mask request plus snapshot identification, snapshot-by-DTC, extended-data, severity, and supported-DTC subfunctions. The data following the selected subfunction remains editable because record identifiers and some layouts vary by ECU.

RoutineControl has its own request builder. The **ECU controls** tab exposes ECUReset, CommunicationControl, WriteDataByIdentifier, and InputOutputControl with an exact payload preview and confirmation before transmission.

Security Access supports requesting a seed and sending key bytes produced by an authorised external algorithm. It deliberately does not include bypass or seed-to-key algorithms.

## Manual requests

The Manual service tab accepts a service byte and arbitrary hexadecimal payload bytes. Responses show the positive response service and payload. Negative responses include the UDS response code description, and Response Pending keeps the request open until the ECU sends its final reply.

Only send diagnostic requests to vehicles and modules you own or are authorised to test.

## Endpoint setup and safety

The addressing selector identifies common 11-bit physical, 11-bit functional, or 29-bit normal-fixed addressing. Changing the selection preserves the request ID, response ID, and response rule you already entered. Choose **Apply defaults** only when you intentionally want the selected type's standard example IDs and response mode. The selection is retained between sessions.

CAN frame width is ultimately inferred from the numeric identifier: IDs through `0x7FF` use the standard 11-bit format and larger IDs use the extended 29-bit format. Response addressing may use an explicit ID, request plus `0x8`, request plus `0x80`, or a custom offset. **Learn response** probes the current request ID and records responding endpoints.

In ECU discovery, **Any response ID** installs a temporary wildcard receive filter. Only frames that decode as a reply to the active UDS probe are recorded. This is useful when neither the 11-bit nor 29-bit response address is known; select the discovered pair and choose **Use selected ECU** afterward.

**Infer from capture** passively finds standard 11-bit and ISO 15765 normal-fixed 29-bit endpoint pairs from captured single-frame UDS replies. Normal-fixed results include decoded source and target bytes. Passive results begin at lower confidence than active positive responses.

Active scans have an inter-request gap and request limit, checkpoint their remaining IDs, and offer to resume after an interrupted run. A newly observed CAN error frame aborts the scan. Multiple replies to one request are marked as ambiguous. **Verify physically** probes only the selected pair. Names and notes are retained in saved results, and loading additional result files merges endpoint pairs rather than clearing the list.

The Setup tab provides Passive, Read-only active, and Full diagnostics safety modes. Passive blocks all diagnostic transmission. Read-only permits discovery and data retrieval but blocks security, clearing, routines, resets, writes, downloads, and controls. Full diagnostics enables those operations while retaining their confirmation dialogs.

P2 controls the normal response timeout and P2* controls Response Pending. ISO-TP flow control is enabled for UDS replies. Block size and STmin are placed directly in generated flow-control frames, and another Continue To Send frame is issued after each configured receive block until reassembly completes. Endpoint profiles retain addressing, session, safety, timing, transport settings, and DID rows.

Each discovery/result page and response pane has its own clear control. Clearing displayed results does not remove configured DID requests or send anything to the ECU.

## Discovery summary

Session discovery probes the common `0x10` session subfunctions. The summary tree combines learned endpoints, sessions, services, and DIDs. It can export CSV and save or compare JSON snapshots to identify additions and missing capabilities.

After session discovery, the workbench sends a request to restore the session selected before the scan.

See [Diagnostic and CAN Tool Feature Roadmap](./diagnostic_feature_roadmap.md) for researched follow-on work and the proposed CANopen workbench.
