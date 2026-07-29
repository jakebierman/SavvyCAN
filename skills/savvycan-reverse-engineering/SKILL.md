---
name: savvycan-reverse-engineering
description: Run passive Sniffer notching and differential CAN experiments, then rank counter, checksum, diagnostic-correlation, and signal-cluster candidates.
---

# SavvyCAN Reverse Engineering

1. Use notching to suppress bits that have not changed since the notch point.
2. Record a stable baseline, the deliberate action, and preferably a control.
3. Stop recording before analysis.
4. Run differential analysis before deeper counter or correlation tools.
5. Report inferred fields as candidates with evidence, never as confirmed facts.
6. Repeat experiments to reject time-correlated or unrelated traffic.
7. When a candidate is sufficiently supported, combine this skill with
   `savvycan-dbc-signals` to add or update the corresponding bitfield, byte
   field, scaling, enum, or multiplex definition.

These operations are passive. Any later test-frame transmission must use a
separate raw CAN or playback capability with the required access controls.
