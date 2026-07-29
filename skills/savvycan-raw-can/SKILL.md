---
name: savvycan-raw-can
description: Prepare, validate, send, repeat, filter, and analyze raw classic CAN frames in SavvyCAN when arbitration IDs and payload bytes are involved.
---

# SavvyCAN Raw CAN

1. Keep the arbitration ID, bus, frame format and payload distinct.
2. Use `frame.send_once` for one explicit transmission.
3. Use `frame.send_loop` for bounded repetition so the row remains visible and
   stoppable in CAN Trace.
4. Use `trace_sender.add` and `trace_sender.update` for compact row drafts.
   Use `trace_sender.start`, `trace_sender.stop`, and
   `trace_sender.send_once` to operate existing rows. Resolve row indices from
   the supplied `trace_sender_rows` application context.
5. Use `trace_sender.copy_selected` to copy the selected trace frame and
   `trace_sender.to_advanced` when received-frame triggers or modifiers are
   needed. The `frame.*` draft/grid capabilities address that advanced sender.
6. Ask for missing identifiers or payloads rather than inventing them.
7. Never claim a frame was sent; SavvyCAN reports the native result.
8. Respect read/full access, arming, validation and confirmation controls.

Classic CAN payloads contain at most eight bytes. Standard identifiers are at
most `0x7FF`; extended identifiers are at most `0x1FFFFFFF`.
