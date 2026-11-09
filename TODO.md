# TODO

## Convention fixes

These are fixes for code that breaks the conventions in `CLAUDE.md` / `components/CLAUDE.md`. Each one is tracked in `review/10-docs-and-conventions.md`, and its status there gets updated in the same change.

**Test harness (needed before the "host tests for pure logic" rule can be followed)**
- Set up host-side unit tests. Options are the IDF `linux` target or plain GoogleTest against the headers.
- First targets:
  - PID (`dt` scaling, derivative cutoff, anti-windup)
  - the motor_controller velocity estimator
  - the protocol codecs: motor_manager configure and telemetry, IMU configure and stream, `time_sync::protocol`
- Wire them into CI together with `idf.py build` for `/` and `test/uart_comm` (review G3, G4, G7).

**Host input validation** (review E1, E2)
- Check host GPIOs against an allowed set: no flash/PSRAM/UART0/USB pins, and nothing ≥ `GPIO_NUM_MAX`.
- Reject non-finite gains, limits and setpoints with an error result code.
- Reject setpoints for unknown motor ids in `queueSetpoint()`, on the RX task, so the control tick never sees them.

**Real-time paths**
- Remove the per-entry `ESP_LOGW` in `applyPendingSetpointsLocked()`. Replace it with a rejected-setpoint counter, and log only a rate-limited summary outside `m_mutex` (review F4).
- Move the encoder HAL's `ESP_LOGE` out of `taskENTER_CRITICAL`: record the error and log it after exiting (review E4).

**Kconfig**
- `time_sync`: task stack size and priority (T3).
- `motor_manager`: config queue depth and mutex timeout.
- `imu_bmo_055`: manager mutex timeout and device lock timeout.
