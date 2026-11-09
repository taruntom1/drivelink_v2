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
