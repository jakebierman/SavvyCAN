# OBD-II Workbench

Open **Send Frames > OBD-II Workbench**, select the CAN bus, and connect. The default functional request ID is `0x7DF`; responses from `0x7E8` through `0x7EF` are collected separately so several ECUs can answer one request.

## Live data

Add Mode 01 PIDs to the table, or use the initial RPM, speed, coolant-temperature, and throttle rows. The editable lookup covers defined standardized Mode 01 PIDs through `0xA6`; select one or type a custom hexadecimal PID. Reserved and manufacturer-specific values remain available through custom entry. Enabled PIDs can be requested once or polled repeatedly.

Known PIDs populate the visible payload-format expression with their standard calculation. Every row can override that expression using the same formatter as the main frame list, for example `u16be/4[rpm]{1}`, `regen:u8&1{0:Inactive,1:Active}`, or a multi-field custom layout. Enter `auto` to use the built-in decoder, or leave an unknown PID on `auto` to display its response bytes as hexadecimal.

See [Payload Formatter Reference](./payload_formatter.md) for the complete custom grammar and the distinction between custom expressions and OBD `auto` decoding.

Use **Save PID list** and **Load PID list** to share or restore the endpoint and complete row definitions as a JSON profile. Custom names, PIDs, enabled states, and format expressions are preserved.

The live graph plots each numeric value produced by the built-in decoder or shared formatter and can export the visible plot as a high-resolution PNG.

**Start trip log** records timestamp, bus, ECU, PID, name, raw bytes, and decoded value to CSV while live requests are running.

The **Trip playback** tab loads those CSV files, supports scrubbing and timed playback, and selects the nearest frame in the main CAN trace using elapsed time from the beginning of each capture. For best alignment, begin the raw CAN capture and diagnostic trip recording together.

## Dashboard

The Dashboard tab is an editable snap-to-grid widget canvas. Choose a standard or custom hexadecimal PID and select **Add widget**. Each widget stores its source PID, title, type, numeric range, optional shared payload format, position, and size.

Available widget types are:

- **Digital readout** for a prominent numeric value.
- **Level** for a vertical fill display between the configured minimum and maximum.
- **Gauge** for a radial value display between the configured minimum and maximum.
- **Text / string** for decoded statuses, enumerations, strings, or multi-part automatic PID output.

Right-click a widget to edit its PID, widget name, type, formatter, and range in one dialog. The widget name defaults to the standard PID name and remains freely editable. Enable **Edit layout** to reveal the grid, then drag a widget to move it or drag its lower-right handle to resize it. Layout changes persist between sessions and can be saved or loaded as versioned JSON. Older fixed-tile layouts are migrated to digital widgets automatically.

The custom calculation field uses the shared payload formatter. Expressions such as `rpm:u16be/4[rpm]{1}`, enums, masks, and `asciiN` strings therefore behave the same way as the main trace and diagnostic request tables. The first formatted field drives numeric gauges and levels; text widgets can display formatted strings or the automatic PID decoder.

Dashboard PIDs must also be present and actively requested on the Live data tab.

## Discovery

**Scan modules** sends a functional Mode 01 PID 00 request and lists every ECU responding from `0x7E8` through `0x7EF`. **Scan available PIDs** checks the standard support bitmap pages from `0x00` through `0xC0` and reports support separately for each ECU. Multi-select results and choose **Add selected to Live data**; recognized PIDs receive their known name and format, while unknown PIDs are added as editable raw/custom rows.

Discovered standard-address ECUs are added to the endpoint target selector. **All ECUs** uses functional request ID `0x7DF`; choosing an ECU derives its physical request ID from the response ID and narrows the response filter. The editable request ID remains available for custom addressing.

Passive safety mode permits listening but blocks requests. Read-only mode permits normal OBD queries and discovery. Full diagnostics is required for Mode 04 clearing and other control requests.

## Freeze frames and monitor tests

The **Freeze frames** tab sends Mode 02 requests for a selected or custom PID and freeze-frame number. Known PIDs use the same automatic calculations as live Mode 01 data, including compound status and temperature results.

**Scan frame PIDs** requests the Mode 02 support bitmap for the selected freeze-frame number and lists the PIDs reported by each ECU.

The **Monitor tests** tab requests Mode 06 results by monitor ID and displays test ID, Unit and Scaling ID, measured value, limits, and pass/fail status. Values remain raw until a source/version-tagged UAS JSON table is loaded. Its `units` object maps hexadecimal UAS identifiers to `factor`, `offset`, `unit`, and optional `signed` fields; conversions then apply consistently to the value and limits.

## Trouble codes

The DTC tab reads stored, pending, and permanent emissions-related trouble codes with Modes 03, 07, and 0A. Mode 04 clearing requires confirmation because it also resets emissions readiness information.

An optional JSON DTC database adds descriptions without tying SavvyCAN to a proprietary dataset. The file contains `source`, `version`, and a `codes` object mapping codes such as `P0300` to descriptions. **Export report** writes the current endpoint, live values, DTCs, freeze frames, monitor tests, and vehicle information to a timestamped text report.

Mode 01 PID `0x01` and PID `0x41` automatically decode MIL/DTC state and supported readiness monitors, including complete/incomplete status and spark/compression-ignition monitor names.

## Vehicle information

The vehicle-information tab sends Mode 09 requests. PID `0x02` requests the VIN, `0x04` requests calibration identifiers, and `0x0A` requests the ECU name where supported.

Only send diagnostic requests to vehicles and modules you own or are authorised to test.

See [Diagnostic and CAN Tool Feature Roadmap](./diagnostic_feature_roadmap.md) for researched follow-on work and the proposed CANopen workbench.
