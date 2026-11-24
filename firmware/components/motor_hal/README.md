# motor_hal

A move-only wrapper for one PWM+DIR DC motor driver on the ESP32-S3 MCPWM. `actuator.pwm_dir` ([mod_motor](../mod_motor/README.md)) uses it through `mod_motor/hardware.hpp`; no other code drives motors.

## API ([`esp_motor.hpp`](include/esp_motor.hpp))

| Call | Contract |
|---|---|
| `Motor(pwm_gpio, dir_gpio[, {.start_forced}])` | Takes an MCPWM operator from the pool and the DIR GPIO. `start_forced` constructs the PWM output forced low. Setup only: the pool is mutex-guarded. |
| `isValid()`, `initError()` | `initError()` returns `ESP_OK` when valid, `ESP_ERR_INVALID_ARG` for bad pins, `ESP_ERR_NOT_FOUND` when the pool is full, `ESP_ERR_INVALID_STATE` for a moved-from object, or the driver error. |
| `write(speed)` | Speed in [-1, 1], clamped; a non-finite speed writes 0. A write with a nonzero compare value sets DIR (high for negative speed) at once, then the compare value, which takes effect at the next PWM period. A zero compare value, including a speed below one timer step, leaves DIR unchanged. No lock or allocation. |
| `forceLow()` | Holds the PWM output low regardless of `write()`. Idempotent register writes with no lock, allocation or log; callable from any task, concurrently with `write()`. |
| `releaseForce()` | Removes the force set by `forceLow()` or `start_forced`. Idempotent. It is not serialized against `forceLow()` on the same motor: the caller must keep a racing release from undoing a stop, as [`actuator.pwm_dir`](../mod_motor/README.md#actuatorpwm_dir) does with its stop epoch. |
| Destructor | Forces the output low before freeing the operator. |

Calls on an invalid or moved-from object return `ESP_ERR_INVALID_STATE`.

## Resources

- Each `Motor` owns one MCPWM operator (six on the ESP32-S3) with one comparator and generator. An operator's two generators share a read-modify-write force register, so operators are never shared: one motor's release must not undo another's force.
- Each MCPWM group's timer is created on demand and shared. A FreeRTOS mutex guards the pool, and limits are enforced in the allocation path.

## Kconfig

"Motor HAL Configuration" in [Kconfig](Kconfig) sets `MOTOR_HAL_PWM_FREQ_HZ` (default 25 kHz) and `MOTOR_HAL_TIMER_RESOLUTION_HZ` (default 10 MHz). The defaults give 400 steps per period, fine enough for the low duty caps of a first motor run.

## Rules

- It returns `esp_err_t` or `bool` and never reports through `diag`; the calling module reports.
- It has not run on a real motor yet; see [Deferred hardware checks](../../../plans/host-status.md#deferred-hardware-checks).
