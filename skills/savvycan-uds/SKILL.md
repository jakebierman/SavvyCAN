---
name: savvycan-uds
description: Operate SavvyCAN UDS ECU, service, session and DID discovery, request lists, polling, DTCs, Tester Present, and guarded diagnostic actions.
---

# SavvyCAN UDS

1. Keep 16-bit DIDs separate from request and response CAN identifiers.
2. Use current endpoint and request-list state for follow-up references.
3. Never infer a manufacturer-specific DID from a description alone.
4. Use native discovery and request capabilities rather than raw frames.
5. Ask for an identifier when the active profile and discovery results do not
   resolve it.
6. Respect session, ISO-TP flow control, access, arming and confirmation gates.

Consult `help/uds_workbench.md` inside the repository for addressing,
discovery, polling and transport details.
