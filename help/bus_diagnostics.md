# Bus Diagnostics

The Bus Diagnostics workspace provides a live per-bus view independent of the selected higher-layer protocol.

- Controller connection, active/disabled, and listen-only state
- Configured nominal bitrate
- Estimated one-second bus load
- Total frame and error-frame counters
- Bus-off, missing-acknowledgement, protocol, arbitration, and controller-error counters
- Timestamped decoded error-frame history with raw driver detail bytes

Bus load includes an allowance for CAN frame overhead and bit stuffing, so it is an estimate rather than a hardware utilization counter. Hardware transmit and receive error counters (TEC/REC) are shown as unavailable when the common SavvyCAN adapter API does not expose them; observed error-frame categories are never presented as TEC/REC.
