# CANopen Workbench

The CANopen Workbench is a CiA 301-oriented view available as a main workspace tab. Select the SavvyCAN bus at the top; received CANopen traffic is decoded continuously.

## Nodes and NMT

Heartbeat and boot-up frames discover nodes passively. **Probe nodes** sends remote node-guarding requests to node IDs 1 through 127. The node list shows the last NMT state, live heartbeat age, identity values read through SDO, and the latest emergency.

Select a node before sending Start, Stop, Pre-operational, Reset node, or Reset communication. Reset commands require confirmation. NMT commands affect real device state and should only be used on an authorized network.

## Object dictionary and SDO

Enter a 16-bit index and 8-bit sub-index, then use **Upload** to read or **Download** to write. The client supports expedited and segmented transfers, plus block transfers when **Block transfer** is selected, and reports standard SDO abort descriptions. Choose the input data type before writing; hexadecimal byte strings, integers, and strings are supported.

Import an EDS or DCF to populate names, data types, access modes, defaults, and configured values. Double-click an object to copy it into the SDO controls. The current dictionary can be exported as a DCF.

An EDS/DCF is a device description, not a live value dump. Importing one creates rows from its object sections; no CAN requests are sent until **Upload**, **Upload selected**, **Download**, or **Write selected** is used.

The list can also be authored manually:

1. Select **Add OD entry** and edit the new row's index, sub-index, name, data type, access, value, and default.
2. Repeat for each required object.
3. Select rows and use **Upload selected** to read their current values from the target node.
4. Edit writable values, select the rows, and use **Write selected** to validate and write them sequentially.

Rows marked `ro` or `const` are skipped by batch writes. Standard EDS numeric data-type codes, common textual integer/string type names, and raw hexadecimal byte values are supported. The authored list can be saved with **Export DCF**.

## PDO and EMCY

The PDO monitor groups live standard TPDO and RPDO COB-IDs, showing node, direction/number, raw bytes, decoded EDS/DCF mappings, frame count, and update time. Mapped numeric values can be graphed. **Remap selected PDO** performs the guarded CiA 301 disable, clear, map, count, and re-enable write sequence; confirm that the selected node and EDS mapping are correct before using it.

The EMCY tab retains timestamped emergency history with node, error code, error register, and manufacturer-specific bytes.

## SYNC and TIME

The network tab monitors the standard SYNC and TIME COB-IDs and can produce either message. Their COB-IDs are editable for networks using configured identifiers.

The LSS tab discovers an unconfigured device and provides a confirmed node-ID/bitrate/store sequence. Use it only with one device in LSS configuration mode. The CiA 402 tab decodes statusword state and exposes confirmed standard controlword transitions.

Block SDO, LSS, PDO remapping, and CiA 402 controls change protocol or device state. Test them against the device documentation on an authorized isolated network first.
