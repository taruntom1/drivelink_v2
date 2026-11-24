# PID math

[`pid_kernel.hpp`](include/pid_kernel.hpp) is header-only, pure and nonlogging, and allocates nothing. `pid::Kernel` is the PID used by `controller.cascade_pid` ([mod_motor](../mod_motor/README.md)): conditional integration under saturation (no windup), derivative on measurement (no setpoint kick) with an optional first-order cutoff, and `track` for a bumpless mode switch.

The gains (`kp`, `ki`, `kd`, `cutoff` in Hz) and all state are public fields of the `Kernel` value, which the caller owns. Each `compute(input, setpoint, dt, limit)` takes elapsed seconds and a symmetric output limit.

- A non-positive or non-finite `dt`, input or setpoint returns 0 and changes nothing. A non-finite output resets the kernel and returns 0.
- The derivative is zero on the first `compute` after `reset()` or `track()`. A `cutoff` of zero or less disables filtering.
- The integral is clamped to ±`limit` and advances only when the output is unsaturated or the step moves it away from saturation.
- `track(input, setpoint, target, limit)` resets the kernel and sets the output-domain integral to `clamp(target) - P`. The next `compute` with the same input and setpoint then returns the clamped target, even with `ki == 0`, provided that integral lies within ±`limit`; otherwise it is clamped and the output steps.

Native tests: [`test_pid.cpp`](../../test/native/tests/test_pid.cpp).
