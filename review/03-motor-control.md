# 03: Motor Control (PID, encoder, motor HAL, motor_manager)

---

## M1. High: production `app_main` runs motors in mock mode

[main.cpp:26](../main/main.cpp): `mgr_cfg.use_mock_feedback = true;`

With mock feedback on, `createMotor()` never constructs a `MotorController`, so no MCPWM or PCNT resources are allocated. `setMode` / `setPidParams` / `configureMotor` become silent no-ops that **still return `Ok`**. Setpoints only move `mock_*` fields, which telemetry then echoes back. The host sees successful acks and plausible telemetry, but no motor turns.

Commit `45815ad` ("mock data false") switched the IMU to real hardware but left this flag on. The Kconfig default (`MOTOR_MANAGER_USE_MOCK_FEEDBACK`, `n`) is overridden by the hard-coded value.

**Fix**: delete the line so the Kconfig default applies. Also have the manager reply `NotSupported` (or set a "mock" flag in responses and telemetry) whenever a hardware-only command is accepted in mock mode.

Also in mock mode, `SetpointKind::OpenLoopDuty` writes to `mock_velocity_rad_s` ([motor_manager.cpp:375-377](../components/motor_manager/src/motor_manager.cpp)), so a duty of 0.5 is reported as 0.5 rad/s. The mock angle is never integrated from velocity, so position telemetry stays frozen.

---

## M2. Medium: PID `dt` comes from config, not from the actual loop timing

- [pid_controller.cpp:160-167](../components/pid/src/pid_controller.cpp): `dt = sample_time_ms / 1000` is used for both the I and D terms.
- [motor_manager.cpp:551-560](../components/motor_manager/src/motor_manager.cpp): `compute()` is called every `update_task.period_ms` (10 ms in `main.cpp`) using `vTaskDelay`, which drifts. It is not `vTaskDelayUntil`.
- `sample_time_ms` is supplied separately by the host in CreateMotor / SetPidParams.

If the host sends `sample_time_ms = 20` while the loop runs at 10 ms, the integral grows **2× too fast** and the derivative is **2× too small**. `compute()` never gates on elapsed time either, so "sample time" means nothing beyond scaling.

**Fix**: either (a) pass the measured `dt` from `esp_timer_get_time()` into `compute()`, or (b) make the update task period *the* sample time: drop the per-motor field or reject a mismatch, and use `vTaskDelayUntil`.

---

## M3. Medium: the encoder rate refreshes at 20 Hz but the velocity loop runs at 100 Hz

[encoder.cpp:26-55](../components/rotary_encoder_hal/src/encoder.cpp). The default `CONFIG_ENCODER_HAL_RATE_SAMPLE_PERIOD_MS` is 50.

`getRate()` returns the cached value until 50 ms have passed. The velocity PID therefore sees the same measurement 5 times and then a step:
- The D term (on measurement) is zero for four cycles and spikes on the fifth.
- The P/I terms react to data up to 50 ms old, which adds phase lag and limits the gain you can use.

**Fix**: compute the rate inside `MotorController::update()` from the count delta since the previous update (the update period is known), with an optional IIR filter. Or set the sample-period Kconfig ≤ the update period and validate that at manager construction.

---

## M4. Medium: the direction pin flips before the PWM duty is updated

[esp_motor.cpp:88-109](../components/motor_hal/src/esp_motor.cpp)

`gpio_set_level(dir, …)` takes effect immediately. The comparator update is latched on TEZ (`update_cmp_on_tez = true`), so up to one PWM period (40 µs at 25 kHz) later. When the sign reverses at high duty, the motor is briefly driven in the new direction at the *old* duty. With some H-bridge drivers this is a current spike. When the sign changes, set duty to 0 first, wait one period (or set the force level), then flip DIR.

---

## M5. Low: other motor_controller / PID notes

| Location | Note |
|----------|------|
| [motor_controller.cpp:20-31](../components/motor_controller/src/motor_controller.cpp) | The PIDs are built with the **unclamped** `config.output_min/max`, while `m_output_min/max` are clamped to [-1,1]. The PID can output values that `setSpeed` later clips, and integral anti-windup clamps to the wrong range. Pass the clamped values. |
| [motor_controller.cpp:162-165](../components/motor_controller/src/motor_controller.cpp) | `m_pos_pid.reset()` followed by `setMode(Automatic)` resets twice (setMode already resets when coming from Manual). |
| [motor_controller.cpp:147-151](../components/motor_controller/src/motor_controller.cpp) | `setMode` stops the motor but leaves `m_open_loop_duty` alone. Switching back to OpenLoop resumes the old duty immediately. |
| [pid_controller.cpp](../components/pid/src/pid_controller.cpp) | Negative gains are clamped in the constructor but *rejected* (ignored with a warning) in `setTunings`. Same input, two behaviours. And `Direction::Reverse` is the only way to get a negative action, but it is not exposed in the UART protocol. |
| [pid_controller.cpp:167-168](../components/pid/src/pid_controller.cpp) | The integral is clamped to the full output range before P/D/FF are added. Large feed-forward plus a full integral saturates for a long time. Consider clamping `integral` to `[out_min - ff, out_max - ff]`, or use back-calculation. |
| Safety | There is no command watchdog. If the host stops streaming setpoints (cable pulled, PC crash), motors keep running at the last setpoint indefinitely. Add a configurable timeout that calls `stop()` when no setpoint or keep-alive arrives. |

---

## M6. Low: motor HAL pool bookkeeping

- [mcpwm_pool.cpp:31](../components/motor_hal/src/mcpwm_pool.cpp): `slot = user_count`. After freeing slot 0 while slot 1 is still in use, the next allocation also gets `slot = 1`. Today the slot is only used in logs, so the logs are misleading. Track a 2-bit occupancy mask instead.
- [mcpwm_pool.cpp:237-239](../components/motor_hal/src/mcpwm_pool.cpp): `MCPWM_TIMER_STOP_EMPTY` is asynchronous, but `mcpwm_timer_disable()` is called immediately afterwards, and the return values are ignored.
- [esp_motor.cpp:28-50](../components/motor_hal/src/esp_motor.cpp): if MCPWM allocation fails, the DIR GPIO stays configured as an output. Reset it with `gpio_reset_pin`.
- Capacity: there are 12 MCPWM channels but only 4 PCNT units, so the real limit is 4 motors. `max_motors` defaults to 8 in `ManagerConfig`. Clamp it to the PCNT limit, or document that `CapacityReached` shows up as `InternalError` when the 5th encoder fails.
