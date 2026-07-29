# CAN Sniffer

Open **RE Tools > Sniffer**. Press **F1** anywhere in the Sniffer window, including while a table, filter, or numeric control has focus, to reopen this page.

![Sniffer Window](./images/Sniffer.png)

## Live view

The Sniffer condenses live traffic to one row per CAN ID, ordered by ID. It is intended for finding changing fields without scrolling through every received frame.

- **Delta** is the time between recent frames for that ID.
- **Frequency** is the estimated update rate.
- **ID** is the CAN identifier.
- **Data 0-7** show the most recently observed classic-CAN payload bytes.
- An incrementing byte or bit is highlighted green; a decrementing one is highlighted red.
- The display refreshes every 200 ms. Frame reception itself is not limited to that rate.

By default, an ID turns red after its configured expiry time and is then removed if no new frame arrives. **Expire Interval** changes that inactivity threshold. **Never Expire IDs** retains inactive rows, which keeps the layout stable during experiments.

Column widths are manually resizable and are restored between sessions when window-position persistence is enabled.

## Filters

The **Filters** list contains observed CAN IDs. Checked IDs are visible when filtering is active.

- **All** selects every observed ID.
- **None** clears every selection.
- Individual checkboxes narrow the view to relevant IDs.

Filtering changes the live presentation. Differential experiment capture records received data frames independently, so use repeatable physical conditions rather than assuming the visible filter limits the experiment samples.

## Notching

Notching suppresses bits that changed during the most recently completed notch interval. This is useful for removing background activity before performing a deliberate action.

1. Leave the system in its baseline state.
2. Select a suitable **Notch Interval**.
3. Press **Notch** repeatedly while unrelated traffic changes.
4. Perform the action of interest.
5. Look for newly changing, non-notched bits.

Each press adds recently changed bits to the existing notch mask. **Unnotch** clears every accumulated notch bit.

**Mute notched bits** prevents changes confined to notched bits from updating the displayed value. This makes new activity easier to see, but the displayed payload is then intentionally incomplete and must not be treated as the exact latest bus value.

**Fade inactive bytes** dims bytes that have not changed recently. **View Bits** replaces each byte display with individual bit cells: stable set bits are dark, stable clear bits are light, newly set bits are green, newly cleared bits are red, and notched bits are grey.

The notch timer and live refresh timer are separate. A longer notch interval captures slower background changes; a short interval isolates faster changes.

## Differential Experiment

Differential experiments compare three labelled frame sets:

1. Press **Baseline** and record the system at rest.
2. Press **Stop** after a representative sample.
3. Press **Action**, perform only the operation being investigated, then press **Stop**.
4. Optionally record **Control** for the same duration and conditions without performing the action.
5. Press **Analyze**.

Starting Baseline, Action, or Control replaces the previous recording for that phase. Only received CAN data frames are retained, up to 250,000 frames per phase. The status line identifies the active phase and displays captured frame counts.

For every CAN ID and bit, analysis compares how often that bit is set in the Action sample against Baseline. When a Control sample exists, changes also seen in Control are subtracted as background. Candidates below a score of 15 are omitted:

```text
score = max(0, abs(action rate - baseline rate)
               - abs(control rate - baseline rate)) * 100
```

The evidence column shows Baseline, Action, and Control set percentages. A high score means the bit changed state distribution during the action; it does not prove meaning, byte order, signedness, scaling, or causation.

Good experiments keep ignition state, bus connections, duration, and unrelated activity consistent. Repeat the same action several times, reverse the action where possible, and use a Control sample when normal background traffic is changing.

## Evidence Window

The **Evidence tools** group is visible directly below **Differential
experiment** in the Sniffer controls. Its four buttons open or reuse the
Reverse-engineering evidence table. The same buttons are repeated along the
bottom of that evidence window.

**Analyze** specifically requires Baseline and Action recordings and replaces
the differential rows. **Counters / checksums**, **Diagnostic correlation**,
and **Signal clusters** can use any retained Baseline, Action, or Control
sample; they do not require all three phases. Columns are manually resizable
and sortable:

- **Analysis** identifies the detector.
- **CAN ID** identifies the candidate frame.
- **Field** identifies a byte or bit.
- **Score** is detector confidence or correlation strength.
- **Evidence** explains the measurement behind the candidate.

The additional analysis buttons append their candidates to the current table.
**Export DBC candidates** becomes useful after at least one detector has
produced evidence rows.

### Counters And Checksums

**Counters / checksums** examines all retained experiment frames:

- Byte and low-nibble values that increment sequentially in at least 70% of their transitions are proposed as counters.
- Bytes matching XOR, XOR complement, additive sum, two's-complement sum, or one's-complement sum in at least 70% of frames are proposed as checksums.

These are lightweight candidate tests, not complete CRC inference. Rolling counters, seeded CRCs, message-ID participation, and manufacturer-specific algorithms require further validation.

### Diagnostic Correlation

**Diagnostic correlation** extracts single-frame positive UDS `0x62` and OBD response services `0x41` through `0x4A`. It compares their first returned numeric bytes with broadcast payload bytes.

Candidates with an absolute Pearson correlation of at least 0.85 are shown as positive or inverse correlations. Sequence position is used for comparison; timestamps are not resampled. Similar sample rates and durations therefore matter, and multi-frame diagnostic values are not currently included by this detector.

### Signal Clusters

**Signal clusters** compares byte-value sequences across different broadcast CAN IDs below `0x700`. Absolute correlation of at least 0.92 creates a candidate. This can reveal duplicated values, gateway copies, related wheel speeds, or shared state changes, but counters and coincidental ramps can also correlate strongly.

### DBC Candidate Export

Select evidence rows and choose **Export DBC candidates**. If nothing is selected, every evidence row is considered. Rows containing a byte field become eight-bit, little-endian, unsigned candidate signals with raw scaling.

The generated DBC is deliberately provisional. Rename signals and verify start bit, length, byte order, signedness, factor, offset, range, units, multiplexing, counters, and checksums before relying on it.

## Suggested Workflow

1. Filter to the relevant bus traffic and enable **Never Expire IDs**.
2. Notch stable background changes.
3. Record controlled Baseline, Action, and Control samples.
4. Analyze and sort by score.
5. Repeat the experiment in both directions.
6. Check counters/checksums before interpreting candidate data bytes.
7. Correlate against diagnostic values where available.
8. Export only repeatable candidates and validate them in the main trace, graphing tools, and DBC editor.
