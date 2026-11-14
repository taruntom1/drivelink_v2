# motor_hal

A move-only wrapper for one PWM+DIR DC motor driver on the ESP32-S3 MCPWM.
`actuator.pwm_dir` (`mod_motor`) uses it through `mod_motor/hardware.hpp`; no
other code drives motors.

## API (`esp_motor.hpp`)

| Call | Use |
|---|---|
| `Motor(pwm_gpio, dir_gpio, {.start_forced})`, `isValid()`, `initError()` | Takes an MCPWM operator from `McpwmPool` and the DIR GPIO. A failed construction keeps its reason for the caller. |
| `write(speed)` | Speed in [-1, 1], clamped; a non-finite speed writes 0. Writes DIR, then the compare value (effective next PWM period). |
| `forceLow()`, `releaseForce()` | Force the PWM output low from any task, and release it. Idempotent. |

## Resources

- Each `Motor` owns one MCPWM operator (6 on the ESP32-S3) with one
  comparator and generator. The two generators of an operator share a
  read-modify-write force register, so operators are never shared: one
  motor's release must not undo another's force.
- Each MCPWM group's timer is created on demand and shared. The pool's state
  is guarded by a FreeRTOS mutex; limits are enforced in the allocation path.

## Kconfig ("Motor HAL Configuration")

`MOTOR_HAL_PWM_FREQ_HZ` (default 25 kHz) and `MOTOR_HAL_TIMER_RESOLUTION_HZ`
(default 10 MHz: 400 steps per period at 25 kHz, fine enough for the low duty
caps of a first motor run).

## Rules

- It returns `esp_err_t`/`bool` and never reports through `diag`; the module
  that calls it reports.
- Not run on a real motor yet: see "Deferred hardware checks" in
  `plans/overhaul-status.md`.
