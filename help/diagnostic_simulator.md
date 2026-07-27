# Diagnostic Replay Simulator

The Diagnostic Simulator creates an isolated virtual CAN connection so the UDS
and OBD-II workbenches can be exercised without a vehicle. Start the virtual bus,
then select its bus number in the diagnostic workbench. Transmitted and received
frames pass through the normal CAN trace and ISO-TP handlers.

## ECU definitions

Each enabled ECU has a request ID, response ID, 11/29-bit selection, response
delay, active UDS session, security level and notes. Multiple ECUs can respond to
functional requests (`0x7DF` or `0x18DB33F1`). ECU state is reset when requested
or with **Reset state**.

The value table belongs to the selected ECU:

- `DID`, key `F190`: returned by UDS ReadDataByIdentifier.
- `PID`, key `01:0C`: returned for OBD mode `01`, PID `0C`.
- `DTC`, key `ALL`: raw UDS DTC records returned by service `0x19`.

Values are hexadecimal bytes. Encoding is descriptive metadata retained in the
project. Writable DIDs accept service `0x2E`; other writes return a negative
response. Apply edits before changing ECU or starting a test.

The UDS emulator includes sessions, resets, tester present, security seed/key,
DID read/write, DTC read/clear, routines, communication and DTC controls, IO
control, and transfer-service acknowledgements. Unsupported services return NRC
`0x11`; unknown identifiers return NRC `0x31`.

## Capture learning

**Learn from capture** identifies ISO-TP single-frame request/response pairs in
the current CAN trace. It creates ECU endpoints and observed DID/PID values while
retaining the existing definitions. Learned entries are proposals and should be
reviewed because a capture cannot prove every state, prerequisite or response.

## Fault injection

Faults can be applied deterministically every Nth response:

- drop a response;
- add latency;
- replace it with a configured negative response;
- create malformed payload data;
- duplicate the response.

Project-defined ECU delay and fault delay are cumulative. Resetting state also
resets the deterministic fault counter.

## Scenarios and reports

Scenario assertions search the timestamped activity evidence. Use
`Activity contains` to require a pattern, or an assertion name containing
`not` to require its absence. Results are marked PASS or FAIL and can be
exported with the activity log as JSON or text.

Simulator projects are portable JSON files containing ECU definitions, values,
fault policy and assertions.

## Safety

The simulator creates a separate virtual bus and does not bridge traffic to
hardware. Confirm the selected bus number before using any diagnostic workbench.
Stopping the simulator removes its connection. Physical forwarding is
intentionally unavailable; use an explicitly configured CAN bridge only when
working on equipment you own or are authorised to test.
