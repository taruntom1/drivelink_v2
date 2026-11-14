# PID math

`pid_kernel.hpp` is header-only, pure and nonlogging, and allocates nothing.
`pid::Kernel` is the explicit-config PID used by `controller.cascade_pid`:
conditional integration under saturation (no windup), derivative on
measurement (no setpoint kick) with an optional first-order cutoff, and
`track`, which presets the output-domain integral for a bumpless mode switch.
Its caller owns all state and supplies elapsed seconds and a symmetric output
limit. A non-positive or non-finite `dt`, input or setpoint returns 0 and
changes nothing. The native tests are in `firmware/test/native/tests/test_pid.cpp`.
