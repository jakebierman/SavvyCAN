---
name: savvycan-capture-playback
description: Load, configure, inspect, and safely replay captured or live CAN traffic through SavvyCAN Playback.
---

# SavvyCAN Capture Playback

1. Load a file with `playback.load_file` or snapshot the current trace with
   `playback.load_live`.
2. Configure bus, timing, burst size and looping before starting playback.
3. Treat play, reverse and single-step operations as transmission.
4. Pause or stop with the native transport controls.
5. Never claim a sequence ran unless the native action result reports success.

Use original timing when reproducing captured timing matters. Use a fixed
interval and bounded sequence loop count for controlled experiments.
