# Plan: timer-driven BNO055 sampling (IMU protocol v3), then `FREERTOS_HZ` = 100

Covers the IMU section of `TODO.md`, the IMU exact-length item under "Convention fixes / Protocol", and these review items:
- `review/04-imu-bno055.md` I4: the timestamp lags the data, and `std::clamp` is UB when min > max
- `review/05-time-sync-and-timestamps.md` T1 / `review/README.md` #6: the IMU and motor streams use different time bases
- `review/10-docs-and-conventions.md`: IMU exact lengths, and the unbounded `portMAX_DELAY` device locks

## Decisions

| Topic | Decision |
|---|---|
| Sampling | A periodic `esp_timer` (task dispatch) calls `notify()` on the stream `StoppableTask`. The task waits in `ulTaskNotifyTake` with a bounded timeout. |
| Timer plumbing | A new shared `rtos_util::TaskTimer` wraps the esp_timer (create, start, restart, stop, and a fenced destroy). motor_manager moves onto it in its own commit, so both use one shutdown path. |
| Sampling vs telemetry | Two periods, as motor_manager's `SetTiming` does. The timer runs at `sample_period_us`, and telemetry goes out every Nth sample. N comes from the host's `telemetry_period_us`, rounded up to a whole multiple. The default N is 1, so today's behaviour doesn't change. |
| Period units | µs throughout: wire, API, Kconfig. |
| Wire format | IMU goes to **v3**. `SetStreamPeriodMs` becomes `SetStreamTiming` (u32 sample period, u32 telemetry period), and the response echoes the applied values. Every configure body must have its exact length. |
| Field mask | Bits outside `kStreamFieldAll` (0x01FF) are rejected with `InvalidArgument`. |
| Timestamp clock | The synced wall clock (`gettimeofday`), the same one motor telemetry uses. |
| Timestamp instant | The midpoint of the I²C read burst. Wall time is read once at the start, and the burst duration comes from `esp_timer_get_time()`, so a time_sync step during the burst can't skew it. |
| Busy device | The stream tick tries the `Bno055Device` mutex with a zero timeout. If it's busy, the tick is skipped and counted. It never waits. |
| Overruns | Separate counters: missed timer periods (`late`), device-busy skips (`busy`) and I²C read failures (`read_err`). A summary is logged at most once per second, only when something changed. |
| Stream disabled | The timer is stopped (and restarted on enable). `SetOperationMode` stops it around the mode switch the same way. |
| Core / priority | No change: the task stays unpinned at priority 6. |
| Tick rate | `CONFIG_FREERTOS_HZ` = 100, after the tick-dependent code is fixed. |
| Delivery | This branch, one commit per layer, each building cleanly with no warnings. One PR at the end. |

## Protocol v3 (IMU)

The configure header is unchanged: `version(=0x03) | seq | subcommand`. Every body must match its exact length, otherwise the result is `InvalidArgument` and nothing is applied.

| Sub | Name | Body | Body length |
|---|---|---|---|
| 0x01 | SetStreamEnabled | `u8 enabled` (0 or 1; any other value is rejected) | 1 |
| 0x02 | **SetStreamTiming** (was SetStreamPeriodMs) | `u32 sample_period_us \| u32 telemetry_period_us` | 8 |
| 0x03 | SetStreamFieldMask | `u16 field_mask` (bits outside 0x01FF are rejected) | 2 |
| 0x04 | SetOperationMode | `u8 mode` (0x00–0x0C) | 1 |
| 0x05 | ReadCalibration | none | 0 |
| 0x06 | WriteCalibration | 41-byte blob (`has_sic` must be 0 or 1) | 41 |
| 0x07 | ApplyCalibration | none | 0 |

**SetStreamTiming**
- A value of 0 keeps the current setting, so `SetStreamTiming(0, 0)` works as a read.
- The sample period is clamped to `[MIN, MAX]` from Kconfig.
- The telemetry period is clamped to `[sample_period, MAX]` and rounded up to a whole multiple of the sample period.
- The firmware stores the **requested** telemetry period. When the sample period changes later, N is recomputed, so the telemetry rate stays as close as possible to what the host asked for.
- On Ok, the response body is `u32 applied_sample_period_us | u32 applied_telemetry_period_us`.

**Configure response**: `version | seq | subcommand | result | [body]`. Only ReadCalibration (41 bytes) and SetStreamTiming (8 bytes) carry a body.

**Stream (0x82)**: the layout is unchanged, with version `0x03`. `timestamp_us` is now wall-clock µs (the time_sync-corrected clock, the same one motor telemetry uses), taken at the midpoint of that sample's I²C reads. It can jump when time_sync steps the clock.

**Note:** in the fusion modes the BNO055 updates its outputs at 100 Hz, so sampling faster than 10 ms only gives new data in the raw (non-fusion) modes. The guide says so.

## Commits

### 1. `rtos_util`: `TaskTimer`
- `TaskTimer` is move-free and non-copyable. Its members:
  - `esp_err_t start(StoppableTask &task, const char *name, uint32_t period_us)`: creates the timer (`ESP_TIMER_TASK` dispatch, `skip_unhandled_events = true`) and starts it periodic. If the timer exists but is stopped, it only restarts it.
  - `esp_err_t setPeriod(uint32_t period_us)`: calls `esp_timer_restart` when the timer is running, otherwise just stores the period.
  - `esp_err_t stop()`: idempotent (`ESP_ERR_INVALID_STATE` is treated as Ok). It does not wait for an in-flight callback, which is harmless because the callback only notifies.
  - `void destroy()` (also called by the destructor): stop, then the one-shot fence that waits out an in-flight callback, then `esp_timer_delete`. Idempotent.
  - `bool isRunning() const` and `uint32_t periodUs() const`.
- The callback only calls `task.notify()`.
- The fence code moves here from `MotorManager::stopControlTimer`. Its fallback delay uses `msToTicksCeil` (commit 4).
- Update `rtos_util/README.md`.

### 2. `motor_manager`: use `TaskTimer`
- `startControlTimer` / `stopControlTimer` and `m_control_timer` are replaced with a `TaskTimer` member. `setTiming` calls `setPeriod`.
- No behaviour or wire change. Update the README's control-task section if it mentions the old functions.

### 3. `imu_bmo_055`: timer-driven sampling and protocol v3
**Kconfig** (µs, each with a `range`)
- `IMU_BMO_055_DEFAULT_SAMPLE_PERIOD_US` = 10000
- `IMU_BMO_055_DEFAULT_TELEMETRY_PERIOD_US` = 0, which means "same as the sample period"
- `IMU_BMO_055_MIN_SAMPLE_PERIOD_US` = 5000, and `IMU_BMO_055_MAX_SAMPLE_PERIOD_US` = 1000000 (this is also the telemetry maximum)
- These replace the three `*_STREAM_PERIOD_MS` options.
- The constructor checks that min ≤ max and fails with `isValid() == false` if not. This fixes the clamp UB from I4.

**Config and API**
- `StreamTaskConfig { enable, sample_period_us, telemetry_period_us, stack_size, priority }`
- `ManagerConfig`: `min/max_sample_period_us`
- `ResultCode setStreamTiming(uint32_t sample_us, uint32_t telemetry_us, uint32_t &applied_sample_us, uint32_t &applied_telemetry_us)` replaces `setStreamPeriodMs`.
- `setStreamFieldMask` returns `InvalidArgument` for unknown bits.
- `StreamCounters getStreamCounters() const` returns `{ late, busy, read_err }`, all `std::atomic<uint32_t>` inside.

**State**
- The stream task reads `m_enabled`, `m_field_mask`, `m_sample_period_us` and `m_telemetry_divider`, which are atomics. The tick never takes the manager mutex.
- `m_mutex` still serializes the writers (config task / public API). It also guards the timer start and stop, so enable, disable and retime can't race.

**Stream task** (`streamTaskLoop`)
1. `n = ulTaskNotifyTake(pdTRUE, timeout)`, where the timeout is twice the sample period plus 100 ms, rounded up to ticks. Check `stopRequested()` after every wake.
2. If `n == 0` while enabled: log a warning, rate-limited, saying the timer hasn't fired. While disabled, the timeout is normal and nothing is logged.
3. `late += n - 1`. Then `countdown -= n`, and `publish = countdown <= 0`. On publish, `countdown += N`, with a floor of 1. Late ticks count toward the countdown, so the telemetry rate holds.
4. Read the sample:
   - real device: `Bno055Device::tryReadSample(mask, sample)` takes the device lock with a zero timeout. `ESP_ERR_TIMEOUT` means `busy++`, any other failure means `read_err++`, and in both cases the tick ends there.
   - mock: `populateMockSample`, with the same timestamp rule.
5. If publishing: encode, then `sendData`. An encode overflow becomes another counter, not a per-tick log.
6. At most once per second, if any counter changed: one `ESP_LOGW` with the deltas, the totals and the period.

On ticks that don't publish, the sample is read and then dropped. That is the hook for a future on-board consumer, such as a latest-sample accessor or a filter. None is built now.

**`Bno055Device`**
- `readSample` becomes `tryReadSample`: the lock is a try-lock, and the timestamp is the burst midpoint (`wall_start + (mono_end - mono_start) / 2`).
- `setOperationMode` / `getOperationMode` replace `portMAX_DELAY` with a bounded wait (`kConfigLockTimeout`, 100 ms; a stream read holds the lock for a few ms at most).
- The per-transaction `ESP_LOGW` in `busRead` / `busWrite` would flood at 100 Hz with the sensor unplugged. Rate-limit it (one line per second plus a suppressed count).

**Enable / disable / operation mode**
- `setStreamEnabled(true)` starts the timer and resets the countdown, so the first tick publishes. `false` stops it.
- `setOperationMode` stops the timer, switches the mode, and restarts the timer if streaming was enabled.
- `setStreamTiming` stores the periods and divider, then calls `TaskTimer::setPeriod`.

**Lifecycle**
- Destructor order:
  1. Reset the callback registration.
  2. `m_stream_timer.destroy()`
  3. `stopTasks()`
  4. Delete the queue and the mutex.
- Every step is safe after a constructor that failed partway.
- `stream_task.enable = false` creates neither the timer nor the task (as today).

**Protocol**
- `kProtocolVersion = 0x03`.
- `parseAndDispatchConfigure` checks exact lengths for every subcommand and validates `enabled` and `has_sic` as 0 or 1.
- `ConfigureSubcommand::SetStreamTiming = 0x02`.
- `sendConfigureResponse` already takes an optional body, so no change there.

### 4. Tick rate: prerequisites, then `FREERTOS_HZ` = 100
**`rtos_util`**
- Add `constexpr TickType_t msToTicksCeil(uint32_t ms)`: rounds up, with a minimum of 1 for `ms > 0`.
- `StoppableTask::sleepUntil`: a `period` of 0 is treated as 1 tick, so it can't spin.

**Fixes**
- `Bno055Device::delayThunk`: `vTaskDelay(msToTicksCeil(ms) + 1)`. The extra tick is there because `vTaskDelay(n)` can return up to one tick early, and Bosch delays are minimums. Before the scheduler starts, it keeps `esp_rom_delay_us`.
- Use `msToTicksCeil` wherever a configurable or small ms value becomes ticks:
  - the I²C transaction timeout (Kconfig range starts at 1 ms)
  - the time_sync response timeout and interval
  - the motor_manager fence fallback delay (10 ms)
- Fixed constants of 50 ms or more stay on `pdMS_TO_TICKS`. They're exact at 100 Hz.

**Config**
- `sdkconfig.defaults`: `CONFIG_FREERTOS_HZ=100`. `test/uart_comm/sdkconfig.defaults` doesn't set it, so it already uses the IDF default of 100.

### 5. Docs
- `imu_bmo_055/docs/protocol-api-guide.md`:
  - v3 tables
  - SetStreamTiming
  - exact lengths
  - the timestamp clock and instant
  - the note on the 100 Hz fusion output rate
  - a "Changes from v2" section
- `imu_bmo_055/README.md`: API, Kconfig, the stream model (timer, divider, counters).
- `docs/internal-architecture.md`, if it describes the stream task.
- `CLAUDE.md`:
  - IMU at v3
  - `SetStreamTiming`
  - the IMU stream is timer-driven
  - `FREERTOS_HZ` is 100, and nothing real-time depends on it
  - `TaskTimer` and `msToTicksCeil` in the concurrency rules
- `review/`:
  - I4: the timestamp-lag and clamp-UB rows are **Fixed**
  - T1 / README #6: **Fixed**, since both streams now use the wall clock
  - `10-docs-and-conventions.md`: the IMU exact-length row is **Fixed**, and the IMU device-lock part of the Kconfig-timeouts row is **Partly fixed** (bounded now, but not in Kconfig)
- `TODO.md`: remove the IMU/tick section and the "IMU: require exact configure body lengths" item.

## Verification
- After each commit: `idf.py build` and `idf.py -C test/uart_comm build`, with no warnings.
- On hardware:
  - IMU at 10 ms sampling with N = 1: stream timestamps are spaced 10 ms ± jitter, and no counters move while idle.
  - Sampling at 5 ms with telemetry at 20 ms: telemetry arrives every 20 ms, and the response echoes `5000 / 20000`.
  - `SetStreamTiming(0, 0)` reads the values back without changing them.
  - `SetOperationMode` while streaming: the stream pauses and resumes, with no `busy` storm and no stall in the stream task.
  - `WriteCalibration` while streaming: `busy` goes up, and the log line is rate-limited.
  - Bad lengths, unknown mask bits, `enabled = 2`: each gets `InvalidArgument` and changes nothing.
  - With `FREERTOS_HZ=100`: the BNO055 init and mode switches work, the motor loop stats (`MOTOR_MANAGER_LOG_LOOP_STATS`) match the 1000 Hz baseline, and time_sync still syncs.
  - IMU and motor timestamps line up on the host.
- The host side has to be updated to IMU v3 separately.

## Out of scope
- I1 (ReadCalibration outside CONFIG mode)
- I2 (silent mock fallback)
- I3 (`bno055_support.c`)
- moving the Bosch files to `vendor/`
- the legacy I²C driver migration
- the shared little-endian helpers
- a Kconfig option for the device lock timeout

## Implementation notes (where the code differs from the plan above)
- `TaskTimer`'s API is `create(task, name)` / `start(period_us)` / `setPeriod` / `stop` / `destroy`, not a single `start(task, name, period)`. The IMU creates the timer at startup and starts or stops it on enable and disable.
- `msToTicksCeil` (`tick_util.hpp`) landed in commit 1, because `TaskTimer`'s fence fallback uses it.
- Docs, `CLAUDE.md`, `review/` and `TODO.md` were updated in the commit that made each change, as the definition of done requires, rather than in a separate docs commit.
- `StreamCounters` also has `encode_errors`. The I²C failure counter is `read_errors`.
- `ImuBno055Manager::stopTasks()` joins the config task (the only other user of the timer) before `m_stream_timer.destroy()`, and only then joins the stream task. `TaskTimer` calls are not thread-safe.
- If a publish tick fails (busy or read error), the next good sample is published, so the telemetry rate holds.
- The `DEFAULT_STREAM_FIELD_MASK` Kconfig range is now `0x000..0x1FF`. The constructor also masks off unknown bits, with a warning.
- **Existing build trees:** `sdkconfig.defaults` only applies when `sdkconfig` is generated. A checkout that already has an `sdkconfig` keeps `CONFIG_FREERTOS_HZ=1000` until that file is deleted (it is regenerated on the next build) or the value is changed in `menuconfig`.
- No host unit tests were added for the new codec and divider logic: the host test harness does not exist yet (`TODO.md`, "Test harness").
