# Configurable Two-Node Observatory

## Scope

Keep the existing AP-associated ESP32-to-host CSI path as the default. Extend
the existing S3/C6 ESP-NOW synchronization path with optional directed peer
probes and a bounded, configurable cadence. Peer traffic remains on the STA
channel. Direct probing is not evidence of coherent RF phase synchronization.

Use the existing node-position configuration for physical geometry. Show
configured sensor locations separately from inferred human locations. Preserve
the current single-occupant calibration, freshness, and vital-quality gates.

## Visualization

Keep sensing frames, authorized vital estimates, and programmable sensing events
as separate message classes. Display current per-node CSI rate, RSSI, freshness,
configured geometry, mesh synchronization, calibration state, and available
inference evidence through the existing API and WebSocket contracts. Never
substitute demo measurements into a live view. Unavailable estimates remain
unavailable rather than being assigned a plausible value.

## Verification

Execute focused Node tests for message dispatch, freshness, and availability.
Compile changed firmware using the installed ESP-IDF toolchain and run nearby
host tests where applicable. Verify live telemetry against nodes 1 and 3 and
check the observatory using a WebGL-capable browser.

## Limits

Firmware source changes are authorized; flashing and provisioning are not.
No change claims medically accurate vitals, validated gesture recognition,
coherent phase fusion, or accurate human localization without measured evidence.
New radio behavior needs physical two-board verification after an explicitly
approved deployment. Existing unrelated worktree changes are preserved.