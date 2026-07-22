# OBD-II Workbench

Open **Send Frames > OBD-II Workbench**, select the CAN bus, and connect. The default functional request ID is `0x7DF`; responses from `0x7E8` through `0x7EF` are collected separately so several ECUs can answer one request.

## Live data

Add Mode 01 PIDs to the table, or use the initial RPM, speed, coolant-temperature, and throttle rows. The editable lookup covers defined standardized Mode 01 PIDs through `0xA6`; select one or type a custom hexadecimal PID. Reserved and manufacturer-specific values remain available through custom entry. Enabled PIDs can be requested once or polled repeatedly.

Known PIDs populate the visible payload-format expression with their standard calculation. Every row can override that expression using the same formatter as the main frame list, for example `u16be/4[rpm]{1}`, `regen:u8&1{0:Inactive,1:Active}`, or a multi-field custom layout. Enter `auto` to use the built-in decoder, or leave an unknown PID on `auto` to display its response bytes as hexadecimal.

See [Payload Formatter Reference](./payload_formatter.md) for the complete custom grammar and the distinction between custom expressions and OBD `auto` decoding.

Use **Save PID list** and **Load PID list** to share or restore the endpoint and complete row definitions as a JSON profile. Custom names, PIDs, enabled states, and format expressions are preserved.

## Discovery

**Scan modules** sends a functional Mode 01 PID 00 request and lists every ECU responding from `0x7E8` through `0x7EF`. **Scan available PIDs** checks the standard support bitmap pages from `0x00` through `0xC0` and reports support separately for each ECU. Multi-select results and choose **Add selected to Live data**; recognized PIDs receive their known name and format, while unknown PIDs are added as editable raw/custom rows.

## Trouble codes

The DTC tab reads stored, pending, and permanent emissions-related trouble codes with Modes 03, 07, and 0A. Mode 04 clearing requires confirmation because it also resets emissions readiness information.

## Vehicle information

The vehicle-information tab sends Mode 09 requests. PID `0x02` requests the VIN, `0x04` requests calibration identifiers, and `0x0A` requests the ECU name where supported.

Only send diagnostic requests to vehicles and modules you own or are authorised to test.
