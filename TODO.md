# TODO

## Convention fixes

These are fixes for code that breaks the conventions in `CLAUDE.md` / `components/CLAUDE.md`. Each one is tracked in `review/10-docs-and-conventions.md`, and its status there gets updated in the same change.

**Error handling**
- Check the ESP-IDF return values that are still ignored (review E3), for example `gpio_set_level`, `mcpwm_comparator_set_compare_value` and `pcnt_unit_get_count` in `Encoder::getCount()`.

**Fail early, leave a consistent state**
- `Motor`: reset the DIR GPIO (`gpio_reset_pin`) when MCPWM allocation fails after it was configured.
- `Bno055Device`: stop reconfiguring an I²C bus it does not own (review I4).

## Error codes over the control UART (`diag`)

Design and decisions: [plans/uart-diagnostics.md](plans/uart-diagnostics.md). One PR per phase. Phase 1 (the `diag` component, the code generator, degraded-mode `main.cpp`, CI checks) is done.

- **Phase 2, managers:** move motor_manager, imu_bmo_055 and time_sync to `DIAG_ERROR/WARN/COUNT/CLEAR`, report the HAL stage that failed in `createMotor` (review E6), and remove the unused IMU event MSG_TYPE.
- **Phase 3, response detail:** add a `detail` byte to the motor and IMU configure responses (both v4), and send a response instead of dropping the command when a config queue is full.
