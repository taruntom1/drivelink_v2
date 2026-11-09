# TODO

## IMU (BNO055): timer-driven sampling, then lower `FREERTOS_HZ`

**Why:** `CONFIG_FREERTOS_HZ=1000` was raised so the motor loop could hit its rate. Once the motor control loop is timer-driven (`plans/esp-timer-control-loop.md`), the IMU stream task is the main code that still depends on tick granularity.

**IMU sampling**
- Drive the IMU stream task the same way as the motor control loop:
  - a periodic `esp_timer` (task dispatch) calls `xTaskNotifyGive` on the stream `StoppableTask`
  - the task waits in `ulTaskNotifyTake` with a bounded timeout
  - the stream period can be changed live with `esp_timer_restart`
- Latch the timestamp at the sampling instant, not when the packet is encoded.
- Count overruns caused by slow I²C transactions or a busy `Bno055Device` mutex, and log them with rate limiting.
- The stream period is currently clamped to `min/max_stream_period_ms` and paced with `sleepUntil` in ticks (`imu_bmo_055.cpp:747`). Move it to µs.

**Then lower the tick rate (to 100 or 250 Hz)**

Prerequisites:
- `bno055_device.cpp:427`: the Bosch delay callback uses `vTaskDelay(pdMS_TO_TICKS(delay_ms))`. Its 1–19 ms delays would round down to 0–1 ticks. Round up to at least one tick, or busy-wait short delays with `esp_rom_delay_us`.
- Look for other `pdMS_TO_TICKS` values below the new tick period. Today they are all 50 ms or more, apart from the one above.
- Add a guard so a `sleepUntil` period that comes out as 0 ticks can't spin.

## Convention fixes

These are fixes for code that breaks the conventions in `CLAUDE.md` / `components/CLAUDE.md`. Each one is tracked in `review/10-docs-and-conventions.md`, and its status there gets updated in the same change.

**Test harness (needed before the "host tests for pure logic" rule can be followed)**
- Set up host-side unit tests. Options are the IDF `linux` target or plain GoogleTest against the headers.
- First targets:
  - PID (`dt` scaling, derivative cutoff, anti-windup)
  - the motor_controller velocity estimator
  - the protocol codecs: motor_manager configure and telemetry, IMU configure and stream, `time_sync::protocol`
- Wire them into CI together with `idf.py build` for `/` and `test/uart_comm` (review G3, G4, G7).

**Namespaces**
- Rename each namespace to match its component, and update all callers and READMEs:
  - `esp_motor` → `motor_hal`
  - `esp_pid` → `pid`
  - `esp_rotary_encoder` → `rotary_encoder_hal`

**Layout**
- `time_sync`:
  - Move `time_sync.cpp` and `time_sync_protocol.cpp` to `src/`, and `time_sync_protocol.hpp` to `priv_include/`.
  - Stop the public `time_sync.hpp` from including the protocol header. Forward-declare, or move the types it needs into a public `time_sync_types.hpp`.
- `imu_bmo_055`:
  - Move `bno055.c`, `bno055.h` and `bno055_support.c` into `vendor/`.
  - Delete `bno055_support.c` or drop it from the build (review I3).

**Host input validation** (review E1, E2)
- Check host GPIOs against an allowed set: no flash/PSRAM/UART0/USB pins, and nothing ≥ `GPIO_NUM_MAX`.
- Reject non-finite gains, limits and setpoints with an error result code.
- Reject setpoints for unknown motor ids in `queueSetpoint()`, on the RX task, so the control tick never sees them.

**Real-time paths**
- Remove the per-entry `ESP_LOGW` in `applyPendingSetpointsLocked()`. Replace it with a rejected-setpoint counter, and log only a rate-limited summary outside `m_mutex` (review F4).
- Move the encoder HAL's `ESP_LOGE` out of `taskENTER_CRITICAL`: record the error and log it after exiting (review E4).

**Protocol**
- Add one shared set of bounds-checked little-endian read/append helpers, for example in `uart_comm`. Use them in all managers instead of local copies and native-endian `memcpy`.

**Kconfig**
- `time_sync`: task stack size and priority (T3).
- `motor_manager`: config queue depth and mutex timeout.
- `imu_bmo_055`: manager mutex timeout and device lock timeout.

**Style**
- `motor_controller.cpp`: rename `TWO_PI` to `kTwoPi`.
- Declare every `TAG` as `static const char *const TAG` (11 files).
