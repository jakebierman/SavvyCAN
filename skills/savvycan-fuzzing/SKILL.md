---
name: savvycan-fuzzing
description: Configure and run bounded SavvyCAN CAN fuzzing experiments with explicit ID ranges, payload size, interval, burst, duration, and safety controls.
---

# SavvyCAN Fuzzing

1. Require a bounded ID range and explicit test parameters.
2. Configure without starting unless the user explicitly requests execution.
3. Use a finite duration and the smallest useful scope.
4. Require armed full access and confirmation before transmitting.
5. Keep emergency stop available and never describe an unconfirmed run as
   active.

Only use on systems the user owns or is authorized to test.
