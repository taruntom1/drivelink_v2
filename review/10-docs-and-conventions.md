# 10: Documentation and Conventions

There is no `CLAUDE.md` in the repo. The governing project conventions are in [.github/copilot-instructions.md](../.github/copilot-instructions.md) and [.github/instructions/hal-component-patterns.instructions.md](../.github/instructions/hal-component-patterns.instructions.md) (`applyTo: components/**`). Findings below quote those rules.

---

## Convention deviations

| Rule (quoted) | Violation |
|---------------|-----------|
| "Check every ESP-IDF return value." | Several call sites ignore return codes. See E3 in [06](06-input-validation-and-error-handling.md). |
| "Clamp user-facing numeric inputs at API boundaries." | Host-supplied GPIOs, gains, setpoints and sample times are not range- or finiteness-checked (E1, E2). |
| "Release resources deterministically in destructors." | Task teardown is timing-based, not deterministic (C1–C3). |
| "Fail early and preserve a consistent object state after failures." | `Motor` leaves the DIR GPIO configured as an output when MCPWM allocation fails. `Bno055Device` reconfigures an I²C bus it does not own (I4). |
| "Guard shared pool mutations with FreeRTOS mutex protection." | Pool mutex creation itself (`ensureInitialized`) is unguarded (C5). |
| "Add Kconfig for tunable defaults and valid ranges." | `time_sync` hard-codes task stack/priority (T3). `motor_manager` hard-codes config queue depth (8) and mutex timeout. |
| Layout: "Use src/ for implementation." | `time_sync` keeps `time_sync.cpp`, `time_sync_protocol.cpp` and the **private** `time_sync_protocol.hpp` at the component root, with no `src/` or `priv_include/`. Yet the public `time_sync.hpp` includes `time_sync_protocol.hpp`, so the protocol header is effectively public. `imu_bmo_055` keeps `bno055.c` / `bno055_support.c` at its root. |
| "Expose an isValid()-style readiness check" | Done everywhere. 👍 |

---

## Documentation drift

| Doc | Says | Reality |
|-----|------|---------|
| [copilot-instructions.md:43](../.github/copilot-instructions.md) | "rotary_encoder_hal: currently minimal/stub component" | Fully implemented quadrature PCNT encoder with a pool. |
| [copilot-instructions.md:54](../.github/copilot-instructions.md) | "There is currently no automated unit test suite" | `test/uart_comm` pytest suite exists. |
| copilot-instructions "Architecture" | Lists only motor_hal and rotary_encoder_hal | Misses uart_comm, motor_manager, motor_controller, pid, time_sync, imu. |
| copilot-instructions "Conventions" | "Preserve current resource model assumptions:" | The sentence ends with a colon and nothing after it. |
| [motor_manager/README.md:196](../components/motor_manager/README.md) | Telemetry `timestamp_us` is "microseconds since boot" | It is `gettimeofday()` wall-clock time (T1). |
| [test_uart_comm_main.cpp:5](../test/uart_comm/main/test_uart_comm_main.cpp) | Test link runs at 921600 baud | The default build is 115200 (see [09](09-testing.md)). |
| [main.cpp:1](../main/main.cpp) | "Example entry point" | This is the production entry point, and it ships with mock motor feedback (M1). |
| [uart_comm.hpp:163](../components/uart_comm/include/uart_comm.hpp) | `sendCommand`: "MSG_TYPE must be in ranges 0x01–0x3F, 0x40–0x7F, or 0xC0–0xFE" | Only the stream range is rejected; 0x00 and 0xFF are accepted (P4). **Fixed.** |
| [uart_comm.hpp:174](../components/uart_comm/include/uart_comm.hpp) | `sendData`: "Fire-and-forget: always succeeds" | Silently drops data if the instance is invalid or the channel is out of range, and truncates `len` without telling the caller. **Fixed:** returns `esp_err_t` and rejects these cases. |

---

## Suggested additions

- A top-level **protocol reference** listing every MSG_TYPE and stream channel in use, from all components, in one table: `0x20/0x60` time sync, `0x21/0x61` motor, `0x22/0x42/0xC2` IMU, `0x80/0x81/0x82` streams. Today it is spread across four Kconfigs. Two components could be configured onto the same type and nothing would notice. A `static_assert` or a startup check in `main.cpp` would catch collisions.
- A short "safety" section in the README: what happens on host disconnect (currently: motors keep running, see M5), on IMU failure (currently: random data, I2), and the mock flags.
