---
name: savvycan-connections
description: Inspect, reconnect, suspend, resume, or configure SavvyCAN CAN adapters, drivers, interfaces, ports, bus speeds, and CAN FD settings.
---

# SavvyCAN Connections

1. Prefer reconnecting an existing profile when it matches the request.
2. Require explicit interface or port details before creating a profile.
3. Keep adapter serial speed, CAN arbitration rate and CAN FD data rate
   separate.
4. Never claim a driver or interface exists unless current state reports it.
5. Treat new connection creation as a guarded operation.
