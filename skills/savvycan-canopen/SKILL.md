---
name: savvycan-canopen
description: Operate SavvyCAN CANopen node discovery, NMT, SDO, Object Dictionary, EDS/DCF, PDO, EMCY, SYNC, TIME, LSS, and CiA 402 workflows.
---

# SavvyCAN CANopen

1. Address Object Dictionary values by node ID, index and subindex.
2. Use EDS/DCF information when loaded, but distinguish definitions from live
   device values.
3. Use native node, NMT, SDO, SYNC, TIME, LSS and drive-state capabilities.
4. Treat writes, reset commands and state-changing NMT operations as guarded.
5. Do not invent an object, data type, access mode or observed node state.

Consult `help/canopen_workbench.md` inside the repository for detailed
workflows and supported transfer modes.
