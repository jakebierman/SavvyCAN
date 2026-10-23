# SavvyCAN Rust migration (in progress)

This directory contains the **new Rust implementation**. The original Qt/C++
SavvyCAN remains in the repository root for feature parity and regression
testing. Nothing here replaces the established application or its release
pipeline yet.

## Why this layout?

- **savvycan-core**: portable pure-Rust CAN frames, validation, candump import,
  DBC loading and decoding, typed payload formatting, and summary metrics. Shared by Slint, CLI tools,
  tests and future adapters. No Qt, Slint or operating-system-specific code.
- **savvycan-transports**: receive-only adapter implementations, beginning with
  Linux SocketCAN (classic, FD and RTR). The adapter exposes no transmit API.
- **savvycan-desktop**: Slint graphical shell. Currently **offline and read-only**.
  Open a supported CAN trace, optionally load a DBC and a typed format, then browse raw and decoded
  frames. Input errors include source line numbers.

## Run

Install a current stable Rust toolchain and Linux GUI development dependencies.

From this directory:

~~~sh
cargo test -p savvycan-core -p savvycan-transports
cargo run -p savvycan-desktop -- /path/to/candump.log /path/to/signals.dbc "rpm:u16be@0/4[rpm]{1}"
~~~

The DBC path, typed format and rules-file path are optional. To pass a format without a DBC, use
an empty string for the DBC argument. You can also enter log/DBC paths and the
formatter expression directly in the UI. Optionally specify a per-ID rules file
as the fourth positional argument or in the viewer. Per-ID rules override the
default typed expression, and RTR requests are never formatted.

## Current feature boundary

| Feature | Rust implementation |
| --- | --- |
| Validated 11/29-bit identifiers, classic CAN and CAN FD payloads | Done |
| Per-interface identity, microsecond timestamps, FD flags and RTR | Done |
| Linux candump compact and simple bracketed-text log import | Done |
| CRTD, GVRET native CSV (V1/V2), generic ID/data CSV imports | Done (content autodetection and source-line errors) |
| BUSMASTER and Microchip Analyzer classic CAN logs | Done (including BUSMASTER RTR and Tx/Rx direction) |
| CARBUS Analyzer V2/V3 text traces | Done (including CR-only line endings) |
| PEAK PCAN `.trc` (1.1/1.2/1.3/2.0/2.1) | Initial support: classic, CAN FD DLC mapping, RTR and Rx/Tx |
| IXXAT MiniLog quoted CSV | Initial support, compatible with legacy SavvyCAN writer (Std/Ext ID and time of day) |
| Vehicle Spy CSV | Initial support: quoted descriptions, ID width, direction, payload and sub-second timestamps |
| Cabana CSV (`time,addr,bus,data`) | Initial support; preserves bus, decimal ID and timestamp with microsecond precision |
| SocketCAN classic PCAP (linktype 227) | Initial import/export support: classic CAN, CAN FD, RTR, micro/nanosecond reads |
| SocketCAN PCAPNG (linktype 227) | Initial import/export: multiple interfaces, endian sections, timestamps and Rx/Tx flags |
| Vector CANalyzer ASC classic CAN frames | Initial support (including RTR; CAN FD records rejected explicitly) |
| CRTD and GVRET native CSV V2 exports | Done for representable classic frames; UI creates new files only |
| Linux candump compact `.log` export (classic, CAN FD, RTR) | Done; timestamps/FD flags retained, Rx/Tx direction unavailable in candump |
| Decoded DBC signals `.signals.csv` export | Done; one row per signal for spreadsheets/plotting |
| Per-interface/CAN-ID `.stats.csv` export | Done; counts, timestamps, interarrival times and estimated frequency |
| Slint traffic-statistics browser | Done; ranked CAN IDs with counts, byte totals, timing and rates |
| Background offline trace import, 1,000-row paging and interface/ID/time/payload filtering | Initial implementation; byte patterns support `??` wildcard and substring matches |
| DBC message/signal load, signed/unsigned Intel/Motorola, scale/offset | Initial support |
| DBC enumerations, IEEE 32/64-bit float, simple and nested mux ranges | Initial support |
| Ordered multiple DBC files and per-interface precedence | Initial support (semicolon-separated viewer paths) |
| DBC J1939 PGN matching | Initial support; enabled by a DBC global protocol attribute; exact ID preferred |
| GMLAN and other DBC protocol profiles | Not yet |
| BLF, other PCAP linktypes, Vector CAN FD ASC and other formats | Not yet |
| Custom typed payload formatter and per-ID rules (separate from DBC) | Initial support: portable parser/router, Slint global and rules-file previews |
| Modular receive-only transport contract and replay mock | Done |
| Passive ISO-TP normal-addressing payload reassembly | Initial core support and selectable read-only Slint inspector (classic/FD SF/FF/CF, sequence and timeout validation); no transmit |
| Passive J1939 BAM multi-packet reassembly | Initial core, Slint inspector and read-only CLI; TP.CM BAM/TP.DT, isolation, sequence/timeout limits |
| Linux SocketCAN classic/FD/RTR receive adapter | Initial support; read-only live capture in Slint |
| GVRET binary receive stream | Initial classic/FD byte parser, tests and opt-in preconfigured serial listener; mode setup and hardware validation not yet |
| LAWICEL/SLCAN receive stream | Initial parser for classic CAN, RTR, optional timestamp and rollover; physical serial attachment not yet |
| SLCAN serial receive attachment | Initial opt-in adapter for preconfigured auto-streaming devices; hardware not yet tested |\n| Native open/save dialogs (trace, DBC, rules, export) | Initial desktop support; selecting an export path never overwrites an existing file |
| GVRET device handshake/activation and TCP, QtSerialBus equivalents, MQTT | Not yet |
| Playback, transmit, scripting, OBD, active UDS, CANopen, native graphing, AI | Not yet |

The frame filter supports a contiguous hex-byte subsequence such as `22 F1 ?? 90`, where `??` matches one byte. Spaces, underscores and commas can separate bytes; matching is case-insensitive, read-only, and excludes remote request frames. Clearing the payload field disables that filter.\n\nThe desktop interface has native open/save path dialogs for CAN traces, multiple DBC files and formatter rules. Selected DBC files populate the semicolon-separated global list; per-interface DBC associations can be entered manually in the same field. Selecting an export filename **does not write anything**: press **Export trace** to create the file, and an existing destination is rejected rather than overwritten. On Linux, the native dialogs require an available XDG Desktop Portal backend at runtime.\n\nOffline file loading is performed on a worker thread so large traces do not block the Slint event loop. Opening another file replaces the pending result; live capture starts cancel the pending result. The worker still reads to completion if its result is abandoned, so cooperative cancellation and streaming disk-to-view loading are future improvements.

Capture import autodetects candump, CRTD, GVRET/generic CSV, BUSMASTER,
Microchip, CARBUS Analyzer V2/V3, PEAK PCAN TRC 1.1–2.1, IXXAT MiniLog
Vehicle Spy and Cabana CSV text logs, plus binary SocketCAN PCAP and PCAPNG captures. The viewer also exports CRTD, GVRET CSV V2 and compact candump `.log` to a user-selected **new** file;
existing paths are never overwritten. Those writers deliberately reject frames they cannot
round-trip (CAN FD, RTR, missing timestamps or unknown Rx/Tx direction) rather
than truncate or invent data. Generic ID/data CSV has no timestamps or direction
and retains those values as unknown. PEAK PCAN event/status/error rows are
skipped because the current CAN-frame model does not represent them. Extended
J1939 records above 64 payload bytes cannot be imported before protocol
reassembly is implemented; unsupported future PCAN TRC versions are rejected
with an explicit error.

The pure-Rust typed formatter supports integer/float byte order, signed values,
text, individual bits, masks and shifts, arithmetic, units, fixed precision,
enum labels, missing-byte markers, validation warnings and repeated fields. A
format is compiled once before opening a trace. Invalid formats fail with a
user-visible error. Per-interface/ID-specific and legacy numeric ID fallback routing are supported
through a text file loaded by the Slint viewer. An interactive per-ID rule
editor is still pending.
Refer to ../help/payload_formatter.md for grammar details.

DBC parsing covers BO_, SG_, VAL_, SIG_VALTYPE_, SG_MUL_VAL_, and global J1939 protocol BA_ directives.
Other metadata is not imported yet. A malformed supported directive or
out-of-range signal is reported rather than silently producing a value.
Extended ID flags use the standard DBC bit-31 convention. Decoding only applies
to data frames, never RTR frames. DBC associations now support an ordered global list plus per-interface files,
with interface matches searched before globals. This still lacks J1939/GMLAN
PGN matching and other advanced DBC profile behavior.

**Safety:** the Rust GUI has no transmit or device-control path. The transport
trait and SocketCAN adapter are deliberately receive-only; no frame transmission
API exists in the Rust migration yet. SocketCAN uses kernel software timestamps where supported, with a
user-space fallback. Hardware timestamp support remains pending. The Slint viewer can open a
Linux SocketCAN interface in a worker thread and stop it without blocking the UI.
Its input queue is bounded; the live display shows the latest 2,000 frames and
live capture retains at most 100,000 frames for export, explicitly reporting
how many older frames were discarded. Hardware integration testing is still
required before the live adapter is considered production-ready. When those are
ported, preserve SavvyCAN's read-only default, access/arming/confirmation policy,
and explicit adapter capability validation.

## Migration order

1. Shared frame/trace types, import fixtures and tests (started).
2. File import/export parity and streaming trace data/filtering.
3. DBC decoding plus the existing typed payload formatter's semantics (started).
4. Adapter trait and transports, beginning with SocketCAN and GVRET,
   preserving CAN FD capabilities and bus-specific settings.
5. Core workbenches and guarded transmit/playback operations.
6. Slint interaction parity, scripting/AI gates, cross-platform packaging.

Build/test workflows are separate from the existing Qt release process.
Do not merge or replace C++ binaries until parity is measured and verified.

## Typed formatter rules file

Each nonempty line assigns a payload expression to an interface and CAN ID.
IDs are hexadecimal; `S:` forces 11-bit, `X:` forces 29-bit, and an
unqualified identifier chooses extended only above 0x7FF. Use `*` for a
numeric-ID fallback when no interface-specific rule exists.

~~~text
# interface CAN-ID custom-format
can0 S:123 rpm:u16be@0/4[rpm]{1} coolant:i8@2-40[C]
can0 X:00000123 status:u8{0:Off,1:On}
* 0x456 voltage:u16le/1000[V]{3}
~~~

The viewer applies matching rules first, then the optional global formatter.
Names like `can0` and `bus0` must match the imported capture's interface
exactly. Neither DBC decoding nor the underlying frame bytes are changed.

## Export limitations

Select an output path ending in `.crtd`, `.crt`, `.csv`, `.log`, `.candump`,
`.pcap`, `.pcapng`, `.stats.csv` or `.signals.csv`
and click **Export trace**. The `.log`/`.candump` exporter supports classic
CAN, CAN FD (including BRS/ESI flags), remote requests, interface identities
and timestamps when present. Candump cannot encode Rx/Tx direction, and that
metadata is therefore lost on re-import; the UI warns when exporting.

CRTD and GVRET CSV are available only for representable classic CAN data frames
with known timestamps, bus numbers and Rx/Tx directions. These legacy exporters
reject CAN FD, RTR and missing metadata rather than silently losing fields.
All exporters validate before creating an output file and never replace an
existing path. `.signals.csv` requires a loaded DBC, exports a separate row for
each decoded signal (including raw and scaled values, units and value labels),
and reports the signal count. Unmatched frames have no decoded-signal rows;
any signal decoding error fails with its frame index before file creation.
DBC-controlled text is quoted and spreadsheet-formula guarded. This format is
for graphing and external analysis, not GVRET re-import.

## Read-only live SocketCAN capture

On Linux, configure a SocketCAN interface (such as `can0` or a test `vcan0`)
before starting SavvyCAN Rust. Enter its name in **Linux SocketCAN interface**
and click **Start receive-only capture**. The worker receives classic CAN, FD
and remote requests, preserving frame identity and FD flags. Press **Stop** to
end the session. No CAN frames are transmitted, and no device configuration
operations are exposed.

The UI refreshes every 100 ms and renders the latest 2,000 frames. The
receive queue holds at most 4,096 pending frames. History is capped at 100,000
received frames; the status line tells you how many older frames have been
discarded. Export writes only those retained frames; choose `.log` to retain CAN FD/RTR
frames and flags, subject to candump's Rx/Tx-direction limitation described above.

## Non-destructive trace filtering

The interface, CAN ID and optional timestamp bounds apply to the displayed rows, without changing
the loaded frames or export data. Export includes every retained frame, not
just the current filter or page. Leave either input blank to match all.
The start/end timestamp inputs are inclusive seconds, with up to six decimal places; frames with unknown timestamps do not match time-bounded searches. Clear the bounds to show them again.
CAN IDs use hexadecimal; `S:123` and `X:123` distinguish 11/29-bit IDs even
when the numeric value is identical. You can also filter `S:100-1FF` ranges,
`X:18FF0000/1FFF0000` ID/mask combinations, and J1939 PGNs such as `PGN:F004`
or `PGN:EA00` (PDU1 destination ignored, PDU2 group extension retained). Offline viewing pages through all matching frames in 1,000-row increments,
without deleting or reducing the imported data. During live capture, the
viewer shows the latest 2,000 matching frames. After stopping capture,
the retained frames can be browsed using the offline page controls.

## CI Linux preview binary

Every migration-branch CI run now tests the Rust core and receive-only
transports, checks the Slint desktop, then builds a runnable Linux x86_64
debug preview. Download `savvycan-rust-slint-linux-x86_64` from the workflow
run's **Artifacts** section and mark the extracted executable as executable.
This is a developer preview and still requires compatible Linux runtime GUI
libraries; it is not a packaged release or an alternative to the existing
Qt/C++ SavvyCAN distributions.

## Multiple DBC associations

Enter semicolon-separated DBC sources in the DBC path field. A bare path adds
a global fallback database. Prefix a path with an interface name and `=` to
associate it with that interface. For example:

~~~text
/path/general.dbc;can0=/path/powertrain.dbc;can0=/path/auxiliary.dbc;can1=/path/chassis.dbc
~~~

For exact CAN IDs, the first matching database wins, with interface-specific
databases searched before the globals. A database explicitly declaring
`BA_ "ProtocolType" "J1939";` (or `BA_ "BusType" "J1939";`) also matches
29-bit identifiers by J1939 PGN. This ignores priority and source address;
for PDU1 messages it also ignores the destination address. Exact IDs are
checked before PGN fallbacks across all databases, and ambiguous PGNs within
a database are left undecoded. Generic extended-ID DBCs retain exact matching
unless the protocol declaration opts in. J1939 transport-protocol reassembly
is not yet implemented. Typed payload formats remain independent.

## Passive ISO-TP groundwork

`IsoTpAssembler` in the portable core reconstructs selected normal-addressed
ISO 15765-2 messages from already captured CAN or CAN FD traffic. Single frames,
multi-frame first/consecutive frames, CAN FD extended single-frame lengths,
32-bit first-frame lengths, sequence rollover, per-interface/CAN-ID/direction
isolation, timeout checks and payload/session limits are supported. Flow-control
records are observed only; nothing is sent on the bus.

A read-only CLI wraps this core API for checking imported trace files:

~~~sh
cargo run -p savvycan-core --bin savvycan-isotp -- /path/capture.log S:7E8 can0
~~~

The ID filter is required; an optional third argument restricts the CAN
interface. The tool prints reconstructed payloads and UDS-like service hints
and warns about incomplete or invalid sequences. The Linux CI preview artifact
now includes this companion executable alongside the Slint desktop binary.

The Slint desktop also has a **Passive ISO-TP** inspector. After loading a trace or stopping capture, enter an exact diagnostic CAN ID such as `S:7E8` or `X:18DAF110`, optionally restrict the interface, and press **Inspect passive ISO-TP**. The separate table displays up to the latest 2,000 reconstructed packets from the full retained history (not just the displayed page or other filters), with truncated 64-byte hex previews, cautious UDS-like service hints, and error/unfinished counts. You must select a diagnostic ID explicitly; patterns and PGNs are rejected to avoid mixing non-diagnostic frames. No requests or flow-control frames are transmitted.

This is groundwork for future diagnostic views, **not** a UDS session-control
or scanning tool. A caller must explicitly select the diagnostic CAN IDs to
reassemble; unrelated CAN traffic can otherwise be mistaken for ISO-TP.
Extended/mixed addressing, full UDS semantic decoding, and ISO-TP transmit remain
outside the current feature boundary.

## IXXAT MiniLog import

The IXXAT quoted-CSV importer preserves 11/29-bit ID format and microsecond
precision from the recorded time of day. The file has no per-frame Rx/Tx or
bus metadata, so direction remains unknown and the interface defaults to
`bus0`. Unsupported nonempty frame flags produce a line-numbered error;
multi-day date reconstruction is not yet implemented.

## Vehicle Spy CSV import

Rows with the standard Vehicle Spy `Line,Abs Time(Sec),...B1` header are
supported. The importer uses the recorded seconds value directly, preserves
Tx/Rx and standard/extended IDs, and parses quoted CSV correctly when
descriptions contain commas. The original network is currently represented
as `bus0`, matching the existing Qt importer. Remote-request rows without a
recoverable DLC are rejected instead of assigning an invented value.

## Binary SocketCAN PCAP

Classic PCAP files with linktype 227 are auto-detected and decoded without
a libpcap runtime. Both file byte orders and microsecond/nanosecond PCAP
timestamp variants are accepted; CAN-ID flags use network byte order.
Classic CAN, CAN FD (BRS/ESI), RTR and capture timestamps are represented.
A `.pcap` export produces classic microsecond-precision PCAP, requires a timestamp
on every frame, and refuses multiple interfaces instead of silently merging
independent buses. PCAP timestamps are interpreted as epoch-based by external
viewers even when the source trace used a relative clock.
PCAP has no standard per-frame interface or Rx/Tx field for this linktype,
so imports default to `bus0` and direction unknown. Truncated records,
error frames, unexpected linktypes and pcapng are rejected explicitly.

## Cabana CSV import

Cabana traces using `time,addr,bus,data` are parsed with decimal CAN IDs,
bus numbers and hexadecimal payload bytes. The recorded timestamp is retained
as seconds/microseconds rather than being rebased to the first frame.
As this CSV does not explicitly identify CAN FD frames or their flags,
payloads beyond eight bytes are rejected until a verified format extension
is available. Direction is unknown rather than assumed received.

## Offline CAN-ID traffic statistics

Click **Traffic statistics** to inspect the top 2,000 interface/ID groups ranked by frame count, including first/last time, mean interarrival and estimated frequency. The table uses all retained frames and is available after loading a file or stopping live capture. Click **Trace frames** to return to the CAN-frame table.

Export to a **new** `.stats.csv` file to get one row per exact interface/ID
combination, including counts of classic/FD/RTR messages, payload bytes,
earliest/latest microsecond timestamps, minimum/maximum/mean interarrival
intervals and estimated frequency. The analysis sorts timestamps within each
ID before calculating intervals. Missing or insufficient timestamps leave
frequency cells blank instead of inventing measurements. This export uses all
retained frames; it does not apply the currently displayed filter or page.

## GVRET binary receive groundwork

`savvycan-transports::GvretStreamDecoder` incrementally parses receive-only
GVRET `F1 00` classic and `F1 14` CAN FD byte streams, preserving the
11/29-bit CAN ID flag, source bus, timestamp in device-relative microseconds,
and 32-bit timestamp rollover. It tolerates arbitrary chunk boundaries,
zero-length frames, binary data containing framing-marker bytes, and known
fixed-length device metadata replies. Malformed identifiers and DLC values
return errors; BRS/ESI/RTR are **not** inferred when the wire record does
not encode them. High CAN FD length bits and unverified 64-byte encodings
are rejected pending firmware-specific validation.

The decoder itself is a byte-stream component. An explicit **Start GVRET\nstream** button opens a preconfigured serial port read-only, but does not enter\nbinary mode (`0xE7`), change bit rates, request device settings, or send CAN\nframes. A GVRET device must already output binary records. Incoming source\nbuses are labelled `bus0`, `bus1`, etc. Serial-port opening may briefly\npulse DTR and reset some microcontrollers. The Slint UI supports receive-only SocketCAN, preconfigured SLCAN serial\nand preconfigured GVRET binary serial streaming. Physical GVRET attachment and hardware tests are required before
claiming a working GVRET device adapter. The legacy implementation and
[M2RET protocol specification](https://github.com/collin80/M2RET/blob/master/CommProtocol.txt)
were consulted for frame layout.

## LAWICEL/SLCAN ASCII receive groundwork

`savvycan-transports::SlcanStreamDecoder` consumes arbitrary serial-byte
chunks and reconstructs CR-terminated `t/T` standard/extended classic CAN
frames and `r/R` remote requests. It handles optional 4-hex-digit
LAWICEL timestamps, unwraps the vendor's documented 60-second millisecond
counter, ignores ordinary command acknowledgement/status text and rejects
invalid IDs, DLCs, payloads and overlong records. Frames without a
recorded timestamp keep it unknown. The parser itself does not transmit anything or open/configure a serial port,
and CAN FD extensions remain unsupported until
their dialect is explicitly identified and tested. See the
[LAWICEL CAN232 manual](https://www.canusb.com/files/can232_v3.pdf).

## Optional receive-only SLCAN serial listener

The Slint desktop now has a **Start SLCAN receive-only** control for an
existing, already configured LAWICEL-compatible serial CAN adapter. Enter the
serial path (e.g. `/dev/ttyUSB0`), UART baud (default 115200) and an
interface label (default `slcan0`). The adapter opens the serial port,
reads its incoming `t/T/r/R` frames, and presents them using the same
bounded live-capture pipeline as SocketCAN. It sends **no serial commands,
CAN frames, bitrate configuration or open/close-bus commands**. The external
adapter must already be in CAN receive/auto-stream mode; this implementation
cannot activate a new device from its default startup state. SLCAN supports
classic CAN and RTR only here, not FD.

**Electrical/device caveat:** serial port opening may briefly assert DTR on
Linux even when configured off in the API; on certain ESP32/Arduino boards
this can reset the microcontroller. Select the port explicitly and confirm
that a reset is safe before clicking Start. This is an experimental adapter;
verify with an isolated test bus, correct adapter firmware and the vehicle's
CAN bitrate before production use. No hardware integration test has run.

The adapter uses `serialport` with `libudev` disabled to reduce Linux
system package dependencies, while preserving standard POSIX/Windows ports.

## Multi-interface SocketCAN PCAPNG import

PCAPNG captures are recognized by their Section Header Block and imported for
`LINKTYPE_CAN_SOCKETCAN` (227). The reader validates section byte order,
block lengths, packet completeness and per-interface configuration; supports
multiple capture interfaces, `if_name` labels, decimal/binary `if_tsresol`,
signed `if_tsoffset`, and Enhanced Packet Block inbound/outbound flags.
CAN FD, classic CAN and RTR use the same validated packet representation as
classic PCAP. Unknown metadata blocks are skipped; unknown linktypes, legacy
Simple Packet Blocks (no timestamp), truncated records and unsupported packet
sizes are explicitly rejected. PCAPNG can also be exported to a **new** `.pcapng` file, preserving
multiple CAN interfaces, Rx/Tx direction, FD/RTR and timestamps. Source frames
without timestamps cannot round-trip and are rejected. Support for additional
linktypes and timestamp-less Simple Packet Blocks remains planned. A modern
[IETF PCAPNG draft](https://datatracker.ietf.org/doc/draft-ietf-opsawg-pcapng/)
was used to validate the format.

## Read-only J1939 broadcast reassembly

`J1939BamAssembler` reconstructs J1939-21 Transport Protocol broadcasts
from captured `TP.CM` BAM (PGN EC00, control 20h) and `TP.DT` (PGN EB00)
frames, preserving the source ECU address, destination application PGN,
source interface and start/end microsecond timestamps. Sessions are isolated
per interface and source, bounded to 1,785-byte payloads, and validated for
announced packet counts, consecutive sequence numbers, fragment time gaps and
reserved fields. Final-chunk padding is discarded. There is no CAN output,
address claiming, RTS/CTS flow control or vehicle control. A read-only CLI can inspect BAM payloads from any supported trace:\n\n~~~sh\ncargo run -p savvycan-core --bin savvycan-j1939-bam -- capture.log 80 can0\n~~~\n\nThe optional source address is hexadecimal, and the optional interface name\nrestricts the trace scan. Output previews the first 48 payload bytes and\nreports invalid/incomplete sessions. The Linux CI artifact includes this\ninspector alongside the ISO-TP CLI. Integration with DBC analysis and the\nSlint diagnostic view remains to do. The frame layout
is verified against [Linux CAN J1939 example traffic](https://github.com/linux-can/can-utils/blob/master/can-j1939-kickstart.md).
