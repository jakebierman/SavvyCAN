---
name: savvycan-obd
description: Operate SavvyCAN OBD-II requests, PID lists, polling, module and supported-PID discovery, DTCs, freeze frames, Mode 06, Mode 09, and dashboards.
---

# SavvyCAN OBD-II

1. Resolve named standard PIDs through SavvyCAN's PID catalog.
2. Treat a PID as a service parameter, never a CAN arbitration identifier.
3. Use `obd.configure_pids` for combined clear, add and start-polling requests.
4. Prefer OBD capabilities over manually constructed raw CAN frames.
5. Use current module, request-list and polling state when resolving references.
6. Require guarded transmission for requests and armed access for clearing DTCs.

Common Mode 01 values include engine speed PID `0x0C`, coolant temperature
PID `0x05`, and vehicle speed PID `0x0D`. Consult
`help/obd2_workbench.md` inside the repository for detailed workflows.
