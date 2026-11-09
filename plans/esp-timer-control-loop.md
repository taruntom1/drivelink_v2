# Plan: esp_timer-driven motor control loop (motor_manager protocol v3)

Covers three items from `review/03-motor-control.md`: a timer-driven loop, the PID `dt` (M2) and the stale encoder rate (M3). It also decouples telemetry from the control rate. `FREERTOS_HZ` stays at 1000 for now; see `TODO.md`.

## Decisions

| Topic | Decision |
|---|---|
| Loop timing | Periodic `esp_timer` (task dispatch). The callback calls `xTaskNotifyGive` on the control task. |
| Control task | `StoppableTask` pinned to core 1 at high priority (Kconfig). |
| Loop rate | Host can set it at runtime with a new `SetTiming` subcommand and change it live. Default comes from Kconfig (1000 µs). |
| PID `dt` | Passed explicitly: `dt = ticks elapsed × period`. `ticks elapsed` is the `ulTaskNotifyTake` count plus any skipped ticks. `sample_time_ms` is removed everywhere. |
| D-term filter | Set as a cutoff in Hz, not a per-sample alpha, so it behaves the same at any loop rate. 0 = off, which is today's default (alpha 1.0). |
| Velocity estimate | `MotorController` computes count delta / dt each tick, then a first-order IIR with a cutoff in Hz (0 = off). Runs in every mode and is cached. |
| Mutex contention | The tick tries the lock with zero timeout. If busy, it skips the tick, counts an overrun, and the next run gets the longer dt. |
| Overruns | Counted and logged (rate-limited). Not in telemetry. |
| Telemetry rate | Sent every Nth control tick. N comes from a host-settable `telemetry_period_us`, rounded up to a whole number of control periods. |
| Telemetry content | Timestamp latched at the sampling tick. New per-motor field: applied output duty. |
| Protocol | motor_manager goes to v3. The IMU stays at v2 and time_sync at v1. |
| Delivery | This branch, one commit per layer, each building cleanly with no warnings. One PR at the end. |

## Protocol v3 (motor_manager)

The configure header is unchanged: `version(=0x03) | seq | subcommand | motor_id`. Configure bodies must now match their length **exactly**, as the setpoint stream already does.

**CreateMotor (0x01) / ConfigureMotor (0x03)**: 64-byte body. The `reserved0/1` floats and `sample_time_ms` are gone.
```
u8  pwm_gpio, dir_gpio, enc_a, enc_b
f32 counts_per_rev, gear_ratio, output_min, output_max
f32 vel_kp, vel_ki, vel_kd, vel_d_cutoff_hz
f32 pos_kp, pos_ki, pos_kd, pos_d_cutoff_hz
f32 velocity_feedforward, position_feedforward
f32 vel_filter_cutoff_hz
```

**SetPidParams (0x05)**: `u8 pid_kind | f32 kp, ki, kd, d_cutoff_hz` (the `u32 sample_time_ms` is removed).

**SetTiming (0x0B, new, global; motor_id ignored)**: `u32 control_period_us | u32 telemetry_period_us`.
- A value of 0 keeps the current setting, so `SetTiming(0, 0)` works as a read.
- The control period is clamped to `[MIN, MAX]` from Kconfig.
- The telemetry period is clamped to `[control_period, 1 s]` and rounded up to a whole multiple of the control period.
- The firmware stores the **requested** telemetry period, so when the control period changes later, N is recomputed and the telemetry rate stays as close as possible to what the host asked for.

**Configure response**: the 5-byte header (`version | seq | subcommand | motor_id | result`) is followed by an optional body that depends on the subcommand. SetTiming's response body is `u32 applied_control_period_us | u32 applied_telemetry_period_us`, sent on Ok. Every other subcommand's response stays at 5 bytes.

**Setpoint stream (0x80)**: layout unchanged, version `0x03`.

**Telemetry stream (0x81)**
- Header: `version(=0x03) | u8 entry_count | u64 timestamp_us`. `timestamp_us` is taken at the control tick that sampled the motors, not when the packet is encoded.
- Each entry: `u8 motor_id | u8 field_mask | [f32 angle] | [f32 velocity] | [f32 output]`.
- New field bit `kTelemetryFieldOutput = 1 << 2`, so `kTelemetryFieldAll` becomes `0x07`.

## Commits

### 1. `rtos_util`: core affinity for StoppableTask
- `start(name, stack, prio, body, BaseType_t core_id = tskNO_AFFINITY)` uses `xTaskCreatePinnedToCore`. Existing callers don't change.
- Update `rtos_util/README.md`.

### 2. `pid` + `motor_controller`: explicit dt and cutoff filters
The PID and motor_controller changes go in one commit, so the API change and its callers build together.

**`pid`**
- `compute(input, setpoint, dt_s, feedforward = 0)`. If `dt_s <= 0`, return the last output without touching state.
- Remove `sample_time_ms`, `setSampleTime()` and `CONFIG_PID_DEFAULT_SAMPLE_TIME_MS`.
- Replace `derivative_filter_alpha` with `derivative_cutoff_hz`. Per call, `alpha = 1 - expf(-2π·fc·dt)`, and `fc = 0` gives `alpha = 1`. Add `setDerivativeCutoff(hz)`.
- Kconfig: `PID_DEFAULT_DERIVATIVE_CUTOFF_HZ`, default 0, which keeps today's behaviour.
- Leave out-of-scope M5 items alone (the integral clamp range and the negative-gain inconsistency).

**`motor_controller`**
- `update(float dt_s)`. Each tick, in every mode:
  1. Read `getCount()` once and compute `raw = Δcount / dt`. The count is accumulator-backed, so the int32 delta is safe.
  2. Convert to rad/s and apply the IIR with `vel_filter_cutoff_hz`.
  3. Cache angle, velocity and the applied output.
- The PID gets `dt_s`. Remove the `ESP_LOGD` calls from the hot path.
- `getAngle()` / `getAngularVelocity()` return the caches, and a new `getOutput()` does the same. Before the first tick, the getters fall back to a direct count read and velocity 0.
- The controller no longer calls `Encoder::getRate()`. The method stays in the HAL for standalone users; its README notes that it's throttled.
- `reset()` / `setMode()` reset the estimator. `prev_count` is set to the current count, so the next tick doesn't produce a spike.
- Config:
  - `MotorControllerConfig::velocity_filter_cutoff_hz`, with Kconfig default `MOTOR_CONTROLLER_DEFAULT_VEL_FILTER_CUTOFF_HZ`.
  - Per-PID `derivative_cutoff_hz`.
  - Setters: `setVelocityFilterCutoff()`, and `setVelocityPidTunings(kp, ki, kd, d_cutoff)` (same for position).
- Remove `setSampleTime()`.
- `motor_manager` call sites are adjusted just enough to build. For now the existing update task passes `period_ms / 1000` as dt.

### 3. `motor_manager`: timer-driven control task
**Config**
- `UpdateTaskConfig` becomes `{ enable, control_period_us, telemetry_period_us, stack_size, priority, core_id }`. Kconfig gets:
  - `MOTOR_MANAGER_CONTROL_PERIOD_US_DEFAULT` = 1000
  - `MOTOR_MANAGER_CONTROL_PERIOD_US_MIN` = 250
  - `MOTOR_MANAGER_CONTROL_PERIOD_US_MAX` = 100000
  - `MOTOR_MANAGER_TELEMETRY_PERIOD_US_DEFAULT` = 10000
  - `MOTOR_MANAGER_CONTROL_TASK_CORE` = 1
  - update task priority default raised to 20 (below the esp_timer task's 22).
- Constructor values are validated and clamped at this boundary.

**Timer**
- `esp_timer_create` with `ESP_TIMER_TASK` dispatch and `skip_unhandled_events = true`, then `esp_timer_start_periodic`. Start it only after the control task is running.
- The callback only loads the task handle and calls `xTaskNotifyGive`. Nothing else.

**Control loop** (`controlTaskLoop`)
1. `n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))`. A timeout (n = 0) means the timer isn't firing: log a warning, rate-limited, and continue. Check `stopRequested()` after every wake.
2. `ticks = n + skipped`, clamped to `kMaxCatchUpTicks` (10) so a long stall can't dump a huge `dt` into the integral. `dt = ticks × period_us × 1e-6`. Any `ticks > 1` adds to the overrun counter.
3. `applyPendingSetpoints()`. The mailbox is unchanged.
4. `xSemaphoreTake(m_mutex, 0)`. If it fails, `skipped += ticks`, count an overrun and go back to step 1.
5. `updateAll(dt)`. On ticks that will publish telemetry, latch `timestamp_us` here.
6. Every N ticks, encode under the lock, release it, then `sendData`.
7. Every 1 s at most, if overruns increased, `ESP_LOGW` the count, the period and the core.

**Live retiming**
- `setTiming()` runs on the config task under `m_mutex`:
  - clamp the values
  - store `m_control_period_us` (atomic) and the requested telemetry period
  - recompute `m_telemetry_divider` (atomic)
  - `esp_timer_restart(timer, period_us)`
- PID `dt` and the filter alphas follow on the next tick, because they are computed from dt. The worst case is one tick that is miscounted.

**Shorter critical sections**
- `createMotor`:
  1. Check the id and capacity under the lock.
  2. Build the `MotorController` **outside** the lock.
  3. Re-lock, re-check, and `push_back`.
- `deleteMotor`: move the `unique_ptr` out under the lock and destroy it outside.
- `configureMotor` / `setPidParams`: move logging out of the lock.

**Lifecycle**
- Destructor order:
  1. Reset the callbacks.
  2. Stop the timer: set `m_timer_stopping`, `esp_timer_stop`, spin until the callback's in-flight counter reaches 0, then `esp_timer_delete`.
  3. `stopTasks()`.
  4. Stop the motors.
- Every step checks for null, so the destructor is safe after a constructor that failed partway.
- `update_task.enable = false` still works. There's no timer; the public `updateAll(dt)` and `publishTelemetry()` are called by the user, and setpoints are applied directly.
- Mock mode still runs the loop so telemetry keeps flowing. M1 (mock-mode problems) is not addressed here.

**Debug option**
- Optional Kconfig `MOTOR_MANAGER_LOG_LOOP_STATS` (default n): log the min, max and mean of the measured period and the loop duration every 5 s. It's useful for checking jitter on hardware.

### 4. `motor_manager`: protocol v3
- Set `kProtocolVersion = 0x03` and use the new configure layouts with exact length checks.
- Add `SetTiming` (0x0B) and `MotorManager::setTiming(control_us, telemetry_us, applied_control_us&, applied_telemetry_us&)`.
- `sendConfigureResponse()` takes an optional body (pointer and length).
- `MotorConfigPayload` loses `sample_time_ms` and gains `vel_d_cutoff_hz`, `pos_d_cutoff_hz` and `vel_filter_cutoff_hz`.
- `setPidParams(id, kind, kp, ki, kd, d_cutoff_hz)`.
- Telemetry: add `kTelemetryFieldOutput`, so `kTelemetryFieldAll` = 0x07, and encode the timestamp latched at the sampling tick.
- `main.cpp`: drop `update_task.period_ms = 10` and use the Kconfig defaults. Leave `use_mock_feedback` as it is; that's M1, a separate change.

### 5. Docs
- `motor_manager/README.md`: rewrite the protocol section for v3 and add a "Changes from v2" note. Describe the timing and telemetry-rate model and the control task (timer, core, overruns).
- `motor_controller/README.md`, `pid/README.md`: describe the `update(dt)` / `compute(…, dt)` API and the cutoff parameters.
- `CLAUDE.md`:
  - motor_manager is at v3 (IMU v2, time_sync v1)
  - add SetTiming
  - the control loop is timer-driven and does not depend on `FREERTOS_HZ`
- `review/03-motor-control.md`: mark M2 and M3 as **fixed**, with a short note on how. Update the matching row in `review/README.md`.
- `TODO.md`: already created. It covers the BNO055 timer-driven sampling and the tick-rate reduction.

## Verification
- After each commit: `idf.py build` with no warnings.
- After commit 4: also `idf.py -C test/uart_comm build`. That app links the shared components, though not motor_manager directly, so this is a sanity check.
- On hardware, with `LOG_LOOP_STATS=y`:
  - A 1 kHz loop with no overruns while idle, and while a configure burst is in progress.
  - Telemetry arrives at the configured rate, and its timestamps are spaced by whole multiples of the control period.
  - `SetTiming` changes (250 µs, 1 ms, 10 ms) apply live, and the response echoes the applied values.
  - A velocity step response at 1 kHz is smoother than before, with no D-term spikes every 50 ms.
- There is no automated motor test yet. The host side has to be updated to v3 separately; nothing in this repo encodes motor_manager frames.

## Implementation notes (where the code differs from the plan above)
- The control-period limits (250 µs to 100 ms) and the 1 s telemetry maximum are `constexpr` in `motor_manager_types.hpp`, not Kconfig options. Only the defaults are in Kconfig.
- Derivative cutoffs have their own setters (`setVelocityDerivativeCutoff` / `setPositionDerivativeCutoff`); there is no extra argument on the tuning setters.
- `setMode()` does not reset the velocity estimator, since velocity is continuous across a mode change. Only `reset()` re-primes it, because that clears the count.
- `applyPendingSetpoints` became `applyPendingSetpointsLocked`, and it runs inside the tick's single try-lock. If the mutex is busy, setpoints stay in the mailbox for the next tick.
- An empty body is required for DeleteMotor, StopMotor and ResetMotor (exact-length rule).

## Out of scope
M1 (mock mode in `main.cpp`), M4 (DIR/PWM ordering), M5 (PID clamp/feed-forward and the watchdog), M6 (pool bookkeeping), the IMU timer loop and the `FREERTOS_HZ` reduction (both in `TODO.md`).
