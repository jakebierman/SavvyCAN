# UDS Workbench

Open **Send Frames > UDS Workbench** to make interactive diagnostic requests through SavvyCAN's existing ISO-TP and UDS support.

Set the CAN bus and request/response IDs, choose a diagnostic session, then select **Connect session**. Tester Present can be enabled to keep that session alive.

## DID requests

Add DIDs to the table, enter each 16-bit identifier, and choose **Request selected** or **Request enabled**. The list, endpoint, and selected session are restored the next time the workbench opens.

Enable **Poll** and select the shared interval to request enabled rows repeatedly. A row's **Poll ms** value overrides the shared interval; use `0` to inherit it. Polling waits while manual requests or an earlier DID queue are active, so only one diagnostic exchange is in flight at a time.

Use **Save DID list** to save the endpoint, session, and DID definitions as JSON. **Load DID list** replaces the current list and endpoint; reconnect the imported endpoint before sending requests.

The Payload format column uses the same formatter as the main frame list. Simple formats such as `u8`, `i16be`, and `f32le` repeat across the returned data. Named or calculated layouts can also be used, for example `rpm:u16be*0.25`. Status values can use enums such as `regen:u8&1{0:Inactive,1:Active}` or add `*:Unknown` as a fallback. Fixed-length strings use `asciiN`, `utf8N`, or `strN`, such as `vin:ascii17`; string fields may also use a byte offset such as `serial:ascii8@20`.

See [Payload Formatter Reference](./payload_formatter.md) for the complete grammar and validation rules.

## Manual requests

The Manual service tab accepts a service byte and arbitrary hexadecimal payload bytes. Responses show the positive response service and payload. Negative responses include the UDS response code description, and Response Pending keeps the request open until the ECU sends its final reply.

Only send diagnostic requests to vehicles and modules you own or are authorised to test.
