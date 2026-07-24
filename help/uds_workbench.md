# UDS Workbench

Open **Send Frames > UDS Workbench** to make interactive diagnostic requests through SavvyCAN's existing ISO-TP and UDS support.

Set the CAN bus and request/response IDs, choose a diagnostic session, then select **Connect session**. Tester Present can be enabled to keep that session alive.

## DID requests

Add DIDs to the table, enter each 16-bit identifier, and choose **Request selected** or **Request enabled**. The list, endpoint, and selected session are restored the next time the workbench opens.

Enable **Poll** and select the shared interval to request enabled rows repeatedly. A row's **Poll ms** value overrides the shared interval; use `0` to inherit it. Polling waits while manual requests or an earlier DID queue are active, so only one diagnostic exchange is in flight at a time.

Use **Save DID list** to save the endpoint, session, and DID definitions as JSON. **Load DID list** replaces the current list and endpoint; reconnect the imported endpoint before sending requests.

The live graph plots every numeric field produced by the shared formatter. CSV logging records timestamped raw and decoded values for later analysis.

The Payload format column uses the same formatter as the main frame list. Simple formats such as `u8`, `i16be`, and `f32le` repeat across the returned data. Named or calculated layouts can also be used, for example `rpm:u16be*0.25`. Status values can use enums such as `regen:u8&1{0:Inactive,1:Active}` or add `*:Unknown` as a fallback. Fixed-length strings use `asciiN`, `utf8N`, or `strN`, such as `vin:ascii17`; string fields may also use a byte offset such as `serial:ascii8@20`.

See [Payload Formatter Reference](./payload_formatter.md) for the complete grammar and validation rules.

## DID discovery

The **DID discovery** tab scans a configurable inclusive range with ReadDataByIdentifier (`0x22`). The default `0xF180` through `0xF1FF` range covers common standardized identification DIDs. Set a timeout appropriate for the ECU and transport; a short timeout makes unsupported DIDs pass quickly, while a slower ECU may require more time.

Only positive responses containing the requested DID are added to the result list. Negative responses and timeouts advance to the next DID, while Response Pending continues waiting for the current DID. The scan can be stopped at any time. Polling pauses during discovery and resumes afterward when enabled.

Select one or more results and choose **Add selected to DID requests**. Existing DIDs are not duplicated. Discovered DIDs use a raw `u8` format by default; VIN DID `0xF190` uses `ascii17`. Review or replace the format after adding a DID because most DID payload definitions are manufacturer-specific.

Large scans require confirmation. DID discovery sends active diagnostic requests and should only be used on modules you own or are authorised to test.

Stopped scans can be resumed in the same session. **Save results** stores the range, next DID, and positive responses as JSON; loading that file restores the exact resume point.

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

See [Diagnostic and CAN Tool Feature Roadmap](./diagnostic_feature_roadmap.md) for researched follow-on work and the proposed CANopen workbench.
