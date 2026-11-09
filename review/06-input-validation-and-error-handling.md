# 06: Input Validation and Error Handling

The host on the UART link is trusted in practice, but it is still an external input. A host bug or a corrupted frame that happens to pass CRC should not be able to brick the board.

---

## E1. Medium: GPIO numbers from the host go to peripherals unvalidated

**Fixed.** `createMotor()` checks the four pins with `gpio_policy::areMotorPinsAllowed()` (they must exist, be distinct, and not be USB, flash/PSRAM or the uart_comm TX/RX pins) and against the pins other motors own. `Motor` and `Encoder` also reject invalid or equal pins before touching hardware, so `1ULL << dir_gpio` can no longer see a value ≥ 64. Strapping pins (0, 3, 45, 46) stay allowed.

- [motor_manager_uart.cpp:145-148](../components/motor_manager/src/motor_manager_uart.cpp): `cfg.pwm_gpio = static_cast<gpio_num_t>(payload[offset++]);` and the same for dir/enc_a/enc_b
- [esp_motor.cpp:30](../components/motor_hal/src/esp_motor.cpp): `io_conf.pin_bit_mask = 1ULL << dir_gpio;`

Problems:
1. `dir_gpio ≥ 64` (any byte 64–255) makes `1ULL << dir_gpio` **undefined behaviour**.
2. On the ESP32-S3, GPIO 26–32 are SPI flash/PSRAM pins and 43/44 are the uart_comm link itself (see `sdkconfig.defaults`). A CreateMotor that names them reconfigures those pins and can crash the chip or cut the host link.
3. Nothing stops PWM == DIR, or `enc_a == enc_b`, or reusing a pin that another motor already owns.

**Fix**: validate in `createMotor` (or better, in the HAL constructors, so every caller benefits):
`GPIO_IS_VALID_OUTPUT_GPIO(pwm)`, `GPIO_IS_VALID_OUTPUT_GPIO(dir)`, `GPIO_IS_VALID_GPIO(enc_*)`, all four distinct, not in a reserved mask (flash, PSRAM, UART, I²C pins), and not already used by an existing slot. Return `InvalidArgument`.

---

## E2. Medium: numeric config from the host is not range-checked

**Fixed.** The motor_manager decoder rejects any non-finite float and any unknown enum value or telemetry field bit with `INVALID_ARGUMENT` (configure) or drops and counts the entry (setpoint stream, checked in `queueSetpoint()` on the RX task, together with unknown motor ids). The C++ boundaries reject them too: `MotorManager::setSetpoint` / `setPidParams` / `createMotor` / `configureMotor`, `PidController::setTunings` / `setOutputLimits` (and its constructor, which falls back to 0 gains or [-1, 1] limits), and the `MotorController` setters, which ignore non-finite values.

| Field | Where it is used | Risk |
|-------|------------------|------|
| `counts_per_rev`, `gear_ratio` | [motor_controller.cpp:38-39](../components/motor_controller/src/motor_controller.cpp) | NaN or Inf pass the `> 0` checks (`NaN > 0` is false, so it falls back to 1.0f, OK). `+Inf` is accepted, and every angle becomes 0. |
| PID gains | [pid_controller.cpp](../components/pid/src/pid_controller.cpp) `setTunings` | NaN passes `< 0` checks. One NaN gain makes the output NaN. `std::clamp(NaN, …)` returns NaN, and `setSpeed(NaN)` ends up computing a compare value from `fabsf(NaN)`, which is undefined behaviour in the cast to `uint32_t`. |
| Setpoints | [motor_manager.cpp:357-403](../components/motor_manager/src/motor_manager.cpp) | NaN/Inf velocity or position setpoints go straight into the PID. |
| `sample_time_ms` | uint32 → `unsigned` | Values in the millions are accepted. See M2. **No longer applicable:** the field was removed in protocol v3. The new filter cutoffs are checked (finite and ≥ 0), and the control and telemetry periods are clamped in `setTiming()`. |

**Fix**: one `isFiniteAndInRange()` gate in `parseAndDispatchConfigure` / `handleSetpointStream`. The project guideline asks for exactly this: "Clamp user-facing numeric inputs at API boundaries".

---

## E3. Low: unchecked ESP-IDF return codes

The project guidelines say "Check every ESP-IDF return value". Places that don't:

| Location | Call |
|----------|------|
| [parser.cpp:78-82](../components/uart_comm/src/parser.cpp) | `uart_read_bytes` when discarding junk (a short read leaves junk in front of the header) |
| [uart_comm.cpp:120,140](../components/uart_comm/src/uart_comm.cpp) | `uart_write_bytes` (a partial or failed TX is invisible; there is no `tx_errors` counter) |
| [esp_motor.cpp:97,107](../components/motor_hal/src/esp_motor.cpp) | `gpio_set_level`, `mcpwm_comparator_set_compare_value` |
| [encoder.cpp:125](../components/rotary_encoder_hal/src/encoder.cpp) | `pcnt_unit_get_count` in `getCount()`: on failure it returns 0, which looks like a valid position |
| [encoder.cpp:164](../components/rotary_encoder_hal/src/encoder.cpp) | `pcnt_unit_clear_count` |
| [mcpwm_pool.cpp:213-250](../components/motor_hal/src/mcpwm_pool.cpp), [pcnt_pool.cpp:207-226](../components/rotary_encoder_hal/src/pcnt_pool.cpp) | All teardown calls in `deallocate()` |
| [time_sync.cpp:157](../components/time_sync/src/time_sync.cpp), [motor_manager.cpp:135-136](../components/motor_manager/src/motor_manager.cpp) | `registerCallback` on unregister. **No longer applicable:** unregistering is now `CallbackRegistration::reset()`, which has no return code to check (see C3). |

---

## E4. Low: logging inside a critical section

[encoder.cpp:36-41](../components/rotary_encoder_hal/src/encoder.cpp): `update_rate_sample_locked()` runs under `taskENTER_CRITICAL` and calls `ESP_LOGE` on failure. Logging from a critical section is not allowed in ESP-IDF: it can block on the console lock, which trips an assert or deadlocks. Record the error code and log after `taskEXIT_CRITICAL`.

**Fixed.** `update_rate_sample_locked()` returns the error, and `getRate()` / `getDirection()` log it after `taskEXIT_CRITICAL`. Previous status: **still open, but no longer on the control path.** `MotorController` no longer calls `Encoder::getRate()` / `getDirection()`; it computes velocity from `getCount()` deltas. The bug remains in the encoder HAL for anyone else who calls those methods.

---

## E5. Low: `app_main` fails silently

**Fixed.** `app_main` never returns. A manager that fails to start is reported to the host as `system.ManagerStartFailed` over the diag event channel, then destroyed, and the others keep running. A failed periodic sync start is reported as `system.PeriodicSyncStartFailed`. If `UartComm` itself fails, the error goes to the console only, and the board idles. See `components/diag/README.md`.

[main.cpp:13-62](../main/main.cpp): every init failure does a bare `return` with the comment `// … failed`, and no log. Then (C1 in [01](01-concurrency-and-lifecycle.md)) the destructors run and may crash. At minimum, `ESP_LOGE` the reason. For a field device, prefer staying alive in a degraded state (UART up, report the error over an event message) so the host can diagnose it.

---

## E6. Low: error codes are lossy

**Fixed.** `createMotor` answers `CAPACITY_REACHED` when the MCPWM or PCNT pool is empty (the HALs keep their `initError()`, and `MotorController::hardwareFailure()` names the stage), and reports `motor_manager.MotorCreateFailed` with the motor id, the stage and the `esp_err` through diag. `sendConfigureResponse` no longer overwrites the result when TX fails: it counts `motor_manager.ResponseTxDropped` and returns the command's own result. The `detail` byte in configure responses (diag plan, phase 3) is still to come.

- `createMotor` returns `InternalError` for every HAL failure, including running out of PCNT units, which is really `CapacityReached`.
- `sendConfigureResponse` returns `InternalError` when TX fails, which overwrites the real result `code` that the caller then logs. The motor operation may have succeeded.
- `Parser` counts an oversized `LEN` as a `crc_error`. **Fixed** by the parser rewrite (P1): it has its own `length_errors` counter.
