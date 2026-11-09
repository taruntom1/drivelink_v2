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
| "Exact lengths. A fixed-size body must be exactly its size." | IMU protocol v2 only checks minimum lengths (`len < N`) in its configure parser ([imu_bmo_055.cpp:607-637](../components/imu_bmo_055/src/imu_bmo_055.cpp)), so trailing bytes are ignored. Fixing it is a wire change, so it needs an IMU protocol version bump. motor_manager v3 and time_sync v1 comply. |
| "Encode and decode field by field with bounds-checked little-endian helpers." | No shared helper exists. IMU has local `readU16Le` / `append*Le` functions. motor_manager `memcpy`s native scalars into telemetry and the `SET_TIMING` response ([motor_manager.cpp:903-954](../components/motor_manager/src/motor_manager.cpp), [motor_manager_uart.cpp:293-294](../components/motor_manager/src/motor_manager_uart.cpp)). That is correct only because the ESP32-S3 is little-endian. |
| Real-time paths: "No logging … while holding `m_mutex`." | `applyPendingSetpointsLocked()` logs one `ESP_LOGW` per rejected setpoint on the control tick while holding `m_mutex` (F4 in [08](08-performance-and-efficiency.md)). The encoder HAL logs inside `taskENTER_CRITICAL` (E4 in [06](06-input-validation-and-error-handling.md)), but the control loop no longer calls that code. |
| Real-time paths: "No heap allocation. That includes copying a `std::function` …" | Compliant today. The RX dispatch copies the callback `std::function` per frame, and only the small-buffer optimisation keeps that copy off the heap (F2 in [08](08-performance-and-efficiency.md)). |
| "Never abort on runtime errors … No `ESP_ERROR_CHECK` anywhere." | Compliant. The only `assert` / `abort` calls guard programmer-error invariants (pool mutex creation, `UartComm` destroyed with live registrations). 👍 |
| "Allocate only at setup or configure time." | Compliant. motor_manager sizes its vectors in the constructor and creates controllers in the create command. 👍 |
| "Release everything deterministically in destructors." | **Fixed** (C1–C3: `StoppableTask`; the motor control timer is also stopped and drained before its task). |
| "Fail early, and leave the object in a consistent state after any failure." | `Motor` leaves the DIR GPIO configured as an output when MCPWM allocation fails. `Bno055Device` reconfigures an I²C bus it does not own (I4). |
| "Guard shared pool state with a FreeRTOS mutex. Create that mutex thread-safely." | **Fixed** (function-local static, see C5). |
| "Put in Kconfig anything a deployment might tune: task stack, priority and core; queue depths; … timeouts." | Hard-coded values remain in three places. `time_sync` sets its task stack and priority in code (`4096`, `6` at [time_sync.cpp:219](../components/time_sync/time_sync.cpp), T3). `motor_manager` hard-codes `kConfigQueueDepth` and `kMutexTimeoutTicks`. `imu_bmo_055` hard-codes `kMutexTimeout` and the 50 ms device lock timeouts. |
| Layout: "`src/`: implementation files … A public header must never include a private one." | `time_sync` keeps `time_sync.cpp`, `time_sync_protocol.cpp` and the private `time_sync_protocol.hpp` at the component root, with no `src/` or `priv_include/`. Its public `time_sync.hpp` includes `time_sync_protocol.hpp`. |
| Layout: "Third-party code lives untouched in a `vendor/` subfolder." | `imu_bmo_055` keeps `bno055.c` and `bno055_support.c` at the component root, and `bno055.h` in its public `include/`. |
| "The namespace equals the component name." | `motor_hal` uses `esp_motor`, `pid` uses `esp_pid`, and `rotary_encoder_hal` uses `esp_rotary_encoder`. |
| "Constants: `constexpr` with a `k` prefix." | `TWO_PI` in [motor_controller.cpp:7](../components/motor_controller/src/motor_controller.cpp). |
| "Declare exactly one module-level tag per source file: `static const char *const TAG`." | One tag per file everywhere 👍. But 11 files declare `static const char *TAG`, a non-const pointer. Only `stoppable_task.cpp`, `parser.cpp` and `uart_comm.cpp` use `*const`. |
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
