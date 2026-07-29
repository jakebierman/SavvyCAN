---
name: savvycan-dbc-signals
description: Create and edit SavvyCAN DBC files, nodes, CAN messages, and decoded signal definitions.
---

# SavvyCAN DBC Signals

1. Identify the loaded DBC by `file_index`.
2. Keep message CAN IDs separate from signal bit positions.
3. Supply byte order, signedness, factor, offset, bounds and unit explicitly.
4. Use `byte_offset` and `byte_length` for byte-aligned fields. Use
   `start_bit` and `bit_length` for arbitrary bitfields.
5. Use `value_type` for integers, floats or strings and `values` for discrete
   enum/status labels.
6. Model multiplexing with `multiplex_role`, `multiplex_values`, and
   `multiplex_parent`; the parent must be a multiplexor in the same message.
7. Use the structured `dbc_files`, selected-frame and Sniffer context before
   changing an existing definition.
8. Do not infer physical scaling without capture or documentation evidence.
9. Remove nodes only after message and signal references have been removed.
10. Save to an explicit path after edits when persistence is requested.

DBC edits alter decoding only and do not transmit CAN traffic.
