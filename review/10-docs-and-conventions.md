# 10: Documentation and Conventions

The governing conventions now live in two places:
- [CLAUDE.md](../CLAUDE.md): real-time paths, protocol, errors and memory, and the definition of done.
- [components/CLAUDE.md](../components/CLAUDE.md): layout, Kconfig scope, API shape, resources, logging, naming and style.

The old `.github/copilot-instructions.md` and `.github/instructions/hal-component-patterns.instructions.md` were reviewed, folded into these files and deleted. Findings below quote the new rules. Every open violation has a matching fix in [TODO.md](../TODO.md).

---

## Convention deviations

| Rule (quoted) | Violation |
|---------------|-----------|
| "Check every ESP-IDF return value." | Several call sites ignore return codes. See E3 in [06](06-input-validation-and-error-handling.md). |
| "Reject invalid host input with an error result code … A valid value outside its range is clamped, and the response reports the value actually applied." | **Partly fixed.** Sample times are gone, the new cutoffs are checked, and the timing periods are clamped and reported. Still open: host GPIOs are not checked against an allowed set (E1), and gains and setpoints are not checked for finiteness (E2). Setpoints for unknown motor ids are accepted into the mailbox and only dropped later, on the control tick (see the real-time row). |
| "Exact lengths. A fixed-size body must be exactly its size." | **Fixed** (IMU protocol v3). IMU v2 only checked minimum lengths (`len < N`), so trailing bytes were ignored. v3 requires every configure body to have its exact length, and rejects out-of-set values (`enabled`, `has_sic`, unknown mask bits). motor_manager v3 and time_sync v1 comply. |
| "Encode and decode field by field with bounds-checked little-endian helpers." | **Fixed.** `uart_comm/include/wire_codec.hpp` provides a shared `Reader` / `Writer` with a sticky error flag. motor_manager (configure, setpoint stream, telemetry, `SET_TIMING` response), the IMU (calibration blob, stream sample, configure) and `time_sync::protocol` use it; the local helpers and native-endian `memcpy`s are gone. The bytes on the wire are unchanged. |
| Real-time paths: "No logging … while holding `m_mutex`." | `applyPendingSetpointsLocked()` logs one `ESP_LOGW` per rejected setpoint on the control tick while holding `m_mutex` (F4 in [08](08-performance-and-efficiency.md)). The encoder HAL logs inside `taskENTER_CRITICAL` (E4 in [06](06-input-validation-and-error-handling.md)), but the control loop no longer calls that code. |
| Real-time paths: "No heap allocation. That includes copying a `std::function` …" | Compliant today. The RX dispatch copies the callback `std::function` per frame, and only the small-buffer optimisation keeps that copy off the heap (F2 in [08](08-performance-and-efficiency.md)). |
| "Never abort on runtime errors … No `ESP_ERROR_CHECK` anywhere." | Compliant. The only `assert` / `abort` calls guard programmer-error invariants (pool mutex creation, `UartComm` destroyed with live registrations). 👍 |
| "Allocate only at setup or configure time." | Compliant. motor_manager sizes its vectors in the constructor and creates controllers in the create command. 👍 |
| "Release everything deterministically in destructors." | **Fixed** (C1–C3: `StoppableTask`; the motor control timer is also stopped and drained before its task). |
| "Fail early, and leave the object in a consistent state after any failure." | `Motor` leaves the DIR GPIO configured as an output when MCPWM allocation fails. `Bno055Device` reconfigures an I²C bus it does not own (I4). |
| "Guard shared pool state with a FreeRTOS mutex. Create that mutex thread-safely." | **Fixed** (function-local static, see C5). |
| "Put in Kconfig anything a deployment might tune: task stack, priority and core; queue depths; … timeouts." | **Fixed.** `time_sync` task stack and priority (`TIME_SYNC_TASK_STACK` / `_PRIO`, via `TimeSyncConfig`, T3), `motor_manager` config queue depth and mutex timeout (`MOTOR_MANAGER_CONFIG_QUEUE_DEPTH`, `MOTOR_MANAGER_MUTEX_TIMEOUT_MS`), and the IMU manager mutex and device lock timeouts (`IMU_BMO_055_MUTEX_TIMEOUT_MS`, `IMU_BMO_055_DEVICE_LOCK_TIMEOUT_MS`) are in Kconfig, with the previous values as defaults. Timeouts are converted with `msToTicksCeil`. |
| Layout: "`src/`: implementation files … A public header must never include a private one." | **Fixed.** `time_sync` sources are in `src/` and `time_sync_protocol.hpp` is in `priv_include/`. The public `time_sync.hpp` now includes only the new public `time_sync_types.hpp` (the default MSG_TYPEs). |
| Layout: "Third-party code lives untouched in a `vendor/` subfolder." | **Fixed.** `bno055.c` and `bno055.h` are in `imu_bmo_055/vendor/` (a private include dir), unchanged. `bno055_support.c` was deleted (I3). |
| "The namespace equals the component name." | **Fixed.** The namespaces were renamed `esp_motor` → `motor_hal`, `esp_pid` → `pid` and `esp_rotary_encoder` → `rotary_encoder_hal`. The header file names (`esp_motor.hpp`, `encoder.hpp`, `pid_controller.hpp`) are unchanged. |
| "Constants: `constexpr` with a `k` prefix." | **Fixed.** `TWO_PI` in motor_controller.cpp was renamed to `kTwoPi`. |
| "Declare exactly one module-level tag per source file: `static const char *const TAG`." | **Fixed.** Every source file now declares `static const char *const TAG` (the 11 component files and the `test/uart_comm` main). |
| "Host tests for pure logic." | There is no host test harness, and no PID or codec tests exist (G3, G4 in [09](09-testing.md)). |
| "Provide an `isValid()` readiness check whenever construction can fail." | Done everywhere. 👍 |

---

## Documentation drift

| Doc | Says | Reality |
|-----|------|---------|
| `.github/copilot-instructions.md:43` | "rotary_encoder_hal: currently minimal/stub component" | Fully implemented quadrature PCNT encoder with a pool. **No longer applicable:** `.github/` was removed. |
| `.github/copilot-instructions.md:54` | "There is currently no automated unit test suite" | `test/uart_comm` pytest suite exists. **No longer applicable:** `.github/` was removed. `CLAUDE.md` describes the test suite. |
| copilot-instructions "Architecture" | Lists only motor_hal and rotary_encoder_hal | Misses uart_comm, motor_manager, motor_controller, pid, time_sync, imu. **No longer applicable:** `CLAUDE.md` covers every component. |
| copilot-instructions "Conventions" | "Preserve current resource model assumptions:" | The sentence ends with a colon and nothing after it. **No longer applicable:** `.github/` was removed. |
| [motor_manager/README.md:196](../components/motor_manager/README.md) | Telemetry `timestamp_us` is "microseconds since boot" | It is `gettimeofday()` wall-clock time (T1). **Fixed:** the README now says `gettimeofday()`. |
| [test_uart_comm_main.cpp:5](../test/uart_comm/main/test_uart_comm_main.cpp) | Test link runs at 921600 baud | The default build is 115200 (see [09](09-testing.md)). |
| [main.cpp:1](../main/main.cpp) | "Example entry point" | This is the production entry point, and it ships with mock motor feedback (M1). |
| [uart_comm.hpp:163](../components/uart_comm/include/uart_comm.hpp) | `sendCommand`: "MSG_TYPE must be in ranges 0x01–0x3F, 0x40–0x7F, or 0xC0–0xFE" | Only the stream range is rejected; 0x00 and 0xFF are accepted (P4). **Fixed.** |
| [uart_comm.hpp:174](../components/uart_comm/include/uart_comm.hpp) | `sendData`: "Fire-and-forget: always succeeds" | Silently drops data if the instance is invalid or the channel is out of range, and truncates `len` without telling the caller. **Fixed:** returns `esp_err_t` and rejects these cases. |

---

## Suggested additions

- A top-level **protocol reference** listing every MSG_TYPE and stream channel in use, from all components, in one table: `0x20/0x60` time sync, `0x21/0x61` motor, `0x22/0x42/0xC2` IMU, `0x80/0x81/0x82` streams. Today it is spread across four Kconfigs. Two components could be configured onto the same type and nothing would notice. A `static_assert` or a startup check in `main.cpp` would catch collisions. *(Partly done: `CLAUDE.md` lists the IDs, the protocol versions and the shared protocol conventions. There is still no collision check.)*
- A short "safety" section in the README: what happens on host disconnect (currently: motors keep running, see M5), on IMU failure (currently: random data, I2), and the mock flags.
