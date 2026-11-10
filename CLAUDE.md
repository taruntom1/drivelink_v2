# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DriveLink v2 is ESP-IDF 5.5.2 firmware for the ESP32-S3, written in C++. It drives DC motors (MCPWM + DIR GPIO), reads quadrature encoders (PCNT), runs PID velocity/position loops, streams BNO055 IMU data, and syncs time with a host. The host talks to the board over a binary UART protocol on UART0 (TX=GPIO43, RX=GPIO44). The console runs on USB-Serial-JTAG, so logs do not mix with protocol traffic.

## Build, flash, test

ESP-IDF must be exported in the shell first:
- PowerShell: `. $HOME\esp\v5.5.2\esp-idf\export.ps1`
- On this machine, the IDF Python env is built for Python 3.11. If `export.ps1` reports a missing `py3.14` env, put `C:\Espressif\tools\idf-python\3.11.2` first on `PATH` and run export again.
- The shell's `IDF_TOOLS_PATH` points at `~\.espressif\Espressif`, which has no `python_env`. `export.ps1` then fails ("ESP-IDF Python virtual environment ... not found"), and `idf.py` is not found. The env is under `~\.espressif`. Shell state does not persist between agent tool calls, so run everything in one PowerShell call:
  ```powershell
  $env:IDF_TOOLS_PATH = "$HOME\.espressif"; $env:PATH = "C:\Espressif\tools\idf-python\3.11.2;" + $env:PATH; . $HOME\esp\v5.5.2\esp-idf\export.ps1 *> $null; idf.py build
  ```

```bash
idf.py build                 # firmware (target esp32s3, from sdkconfig.defaults)
idf.py build flash monitor
idf.py menuconfig            # all component tunables live under their Kconfig menus
idf.py fullclean             # when target/config changes leave the build inconsistent
```

There are two kinds of automated tests.

**Host unit tests** (`test/host/`) cover the pure logic: the wire codec, the motor_manager / IMU / time_sync / diag protocol codecs, diag's slot table, the PID and the velocity estimator. It is a plain CMake project with a vendored doctest (`third_party/doctest`). It compiles those firmware sources directly with the host compiler. `shim/` stands in for the two IDF headers they include (`sdkconfig.h` with the Kconfig defaults they read, and a no-op `esp_log.h`). No IDF export is needed; on this machine, Strawberry Perl ships `g++`, `cmake` and `ninja`.

```bash
cmake -S test/host -B test/host/build -G Ninja
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure   # or run test/host/build/host_tests directly
test/host/build/host_tests -tc="*SetTiming*"          # doctest filter
```

A source can join the host build only if it builds without ESP-IDF beyond those shims. Keep codecs and math in their own files (`*_protocol.cpp`, `velocity_estimator.cpp`), apart from the code that touches FreeRTOS or drivers, and add them to `FIRMWARE_SOURCES` in `test/host/CMakeLists.txt`. If one reads a new `CONFIG_*` symbol, add it to `shim/sdkconfig.h`.

**CI** (`.github/workflows/ci.yml`) runs on every pull request and on pushes to `main`:
- `idf.py build` in the `espressif/idf:v5.5.2` container, which fails on any compiler warning. It builds `/`, then `/` again with `sdkconfig.ci.nolog` (`CONFIG_DIAG_CONSOLE_LOG=n`), then `test/uart_comm`.
- On Ubuntu, `tools/gen_diag_codes.py --check` and the host tests.

**diag codes** are defined in `components/diag/diag_codes.yaml`. After editing it, regenerate and commit the outputs; CI fails if they are stale. The script needs PyYAML, which the IDF Python env and this machine's Python both have:

```bash
python tools/gen_diag_codes.py
```

**Hardware-in-the-loop tests** for `uart_comm` live in `test/uart_comm/`, a separate IDF app whose `EXTRA_COMPONENT_DIRS` points at `../../components` and that builds only the components `main` requires (`set(COMPONENTS main)`).

```bash
idf.py -C test/uart_comm build flash           # flash the test firmware
cd test/uart_comm && pip install -r requirements.txt
pytest pytest_uart_comm.py --port COM5                      # USB-UART adapter on GPIO43/44
pytest pytest_uart_comm.py --port COM5 -k test_echo_max_payload   # single test
```

`conftest.py` defaults `--baud` to 115200, which matches the Kconfig default, even though some comments say 921600. Everything else is checked by a clean `idf.py build` (there are no warnings today, so keep it that way) and by watching runtime logs.

## Architecture

**Startup** (`main/main.cpp`):
- The order is `diag::init()`, `UartComm`, then `MotorManager`, `TimeSync` and `ImuBno055Manager` (each takes a reference to `UartComm`), then `diag::Reporter`.
- Each manager is checked with `isValid()`. One that failed is reported (`system.ManagerStartFailed`), destroyed through its `std::optional`, and left out, so destructors must be safe at any point. The rest keep running.
- The Reporter comes last so its `Boot` event carries the final `started_mask`. Records from the startup wait in the diag queue until then.
- `app_main` never returns. If `UartComm` fails, there is no link to report on: it logs to the console and idles.

**Layering** (each one is an ESP-IDF component under `components/`):
- `motor_hal` (`motor_hal::Motor`) and `rotary_encoder_hal` (`rotary_encoder_hal::Encoder`) are move-only hardware wrappers. Scarce peripherals come from static pools (`McpwmPool`, `PcntPool`) guarded by a FreeRTOS mutex.
- `pid` is pure math.
- `motor_controller` combines one motor, one encoder and the PIDs into a controller with open-loop, velocity and position modes. Its velocity estimate is the pure `VelocityEstimator`.
- `motor_manager`, `imu_bmo_055` and `time_sync` are the protocol-facing managers. Each registers UART callbacks and owns its own tasks. Each keeps its payload codec in a pure module with no ESP-IDF dependency (`motor_protocol`, `imu_protocol`, `time_sync_protocol`, all private), built on `uart_comm::wire::Reader` / `Writer`. The managers decode and encode only through these, so the host tests cover the wire format.
- `rtos_util` provides `StoppableTask`.
- `diag` reports errors to the host as codes. The `DIAG_*` macros latch a per-code slot and queue a record, and `diag::Reporter` sends events and answers queries over `uart_comm`. Only the managers and `main` report. The hardware layers return errors and don't depend on `diag`, and the reporter reads `uart_comm`'s link counters itself. Its codec and slot table are pure modules (`diag_protocol`, `diag_table`).

**UART protocol** (`uart_comm`):
- Frame layout: `AA AA AA | MSG_TYPE | LEN | payload | CRC16-CCITT(0x1021, init 0xFFFF)`. The CRC covers MSG_TYPE, LEN and the payload.
- RX framing is done in software (`Parser`); hardware pattern detection is not used. Driver events only wake the RX task, and `Parser::nextFrame()` pulls bytes from the ring buffer and scans for the preamble. The preamble is not escaped, so a rejected candidate (bad LEN or CRC) drops one byte and rescans. Never flush the RX buffer: it may already hold the next valid frames.
- MSG_TYPE ranges: commands `0x01–0x3F`, responses `0x40–0x7F`, streams `0x80–0xBF`, events `0xC0–0xFE`.
- TX has two paths. `sendCommand()` uses a FIFO queue that is drained first. `sendData()` writes to per-channel stream slots where only the latest value is sent.
- RX either calls a callback registered for the MSG_TYPE, on the RX task, or pushes the frame to a queue read by `receive()`.
- Message IDs and channels come from each component's Kconfig:
  - time_sync: `0x20` / `0x60`
  - motor_manager: `0x21` / `0x61`, setpoint stream `0x80`, telemetry `0x81`
  - IMU: `0x22` / `0x42`, stream `0x82`
  - diag: query `0x23` / `0x63`, events `0xC0`
  - Nothing checks for collisions between these IDs.
- Every protocol payload starts with a version byte (`kProtocolVersion`).
  - motor_manager and IMU are both at v3. Both have their configure commands and responses carry a host-chosen `seq` byte right after the version, echoed in the response. A response to a header that could not be parsed uses `0xFF` (`kUnparsedHeaderByte`) for subcommand (and motor_id). Both require exact configure body sizes.
  - motor_manager: `SET_TIMING` (`0x0B`) sets the control and telemetry periods; its response appends the applied values after the 5-byte header.
  - IMU: `SetStreamTiming` (`0x02`) sets the sample and telemetry periods; its response appends the applied values after the 4-byte header.
  - Motor and IMU stream timestamps are both wall-clock µs (`gettimeofday`, corrected by time_sync), latched at the sampling instant.
  - time_sync is at v1. Its ESP-initiated request already carries a `u16 request_id`.

**Concurrency rules**:
- **Task lifecycle:** create every task with `rtos_util::StoppableTask`. Never use bare `xTaskCreate` / `vTaskDelete`.
  - Task bodies loop on `stopRequested()` and sleep with `sleepFor` / `sleepUntil`.
  - Every other blocking wait needs a bounded timeout.
- **Tick rate:** `CONFIG_FREERTOS_HZ` is 100 (10 ms ticks). Nothing real-time depends on it; fixed-rate loops use `esp_timer` (see "Periodic timers").
  - `pdMS_TO_TICKS` rounds down, so any value below 10 ms becomes 0 ticks. Convert small or configurable ms values with `rtos_util::msToTicksCeil` (`tick_util.hpp`).
  - `vTaskDelay(n)` can return up to one tick early. Where a delay is a minimum (sensor settling), use `msToTicksCeil(ms) + 1`.
  - Owners call `stop()` in their destructor, which waits for the body to return, before freeing anything the task uses.
- **UART callbacks:** `registerCallback(type, cb, registration)` fills in a `uart_comm::CallbackRegistration`, which unregisters on destruction. Unregistering waits for a callback that is already running.
  - Destructors reset their registrations **first**.
  - Registrations must not outlive the `UartComm` (its destructor aborts if one is still active).
  - Callbacks run on the RX task, so they must not block. Hand work off through a queue instead: managers forward configure commands to their own config task queue.
  - Setpoint streams go into a mailbox that the motor update task applies on its next tick.
- **Motor control loop:**
  - A periodic `esp_timer` (task dispatch) wakes the motor_manager update task, which is pinned to core 1 at priority 20. The loop does not depend on `CONFIG_FREERTOS_HZ`.
  - Each tick passes `dt = elapsed ticks × period` to `MotorController::update(dt)` and on to the PIDs. Nothing stores a sample time.
  - The tick only try-locks the manager mutex: a busy mutex skips the tick (counted as an overrun) rather than blocking it. Keep work done under `m_mutex` short.
  - Shutdown stops the timer and waits out an in-flight callback before stopping the task.
- **Periodic timers:** fixed-rate loops (the motor control loop, the IMU sampler) use `rtos_util::TaskTimer`: a periodic `esp_timer` whose callback only notifies a `StoppableTask`. The task takes the count with `ulTaskNotifyTake` (bounded timeout) and counts extra periods as overruns. Owners serialize timer calls under their mutex and call `destroy()` (fenced) before stopping the task.
- **IMU sampling loop:**
  - The `TaskTimer` wakes the IMU stream task every sample period. A packet is sent every N samples, with N derived from the telemetry period. The loop does not depend on `CONFIG_FREERTOS_HZ`.
  - The tick reads stream settings from atomics and only try-locks the `Bno055Device` mutex. A busy device skips the tick and counts it. Late, busy, read-error and encode-error counts are diag counters (`DIAG_COUNT`), and nothing is logged per tick.
  - The timer is stopped while the stream is disabled and during an operation-mode switch.
- **Wake-up frames:** managers wake their config task with a dummy queue item on shutdown. The config task must re-check the stop flag before dispatching anything it receives.

## Conventions

This file holds the project-wide conventions. Component rules (layout, Kconfig scope, API shape, resources, logging, naming and style) are in [components/CLAUDE.md](components/CLAUDE.md). Code that breaks a rule today is listed in `review/10-docs-and-conventions.md`, and its fix is in `TODO.md`.

### Real-time paths

These paths are real-time:
- the motor control tick (`MotorManager::updateTaskLoop` work, `MotorController::update`, `PidController::compute`)
- `esp_timer` callbacks
- ISRs and PCNT/MCPWM driver callbacks
- code inside `portENTER_CRITICAL` or a spinlock
- UART RX callbacks

Rules for them:
- **No logging** in ISRs, critical sections or spinlocks, or while holding `m_mutex`. Anywhere else on these paths, logging must be rate-limited. Report counts instead: an overrun counter, a rejected-entry counter.
- **No heap allocation.** That includes copying a `std::function` whose capture might not fit its small buffer.
- **No blocking.** Only zero-timeout try-locks and non-blocking queue operations are allowed. Waiting belongs in the task's own wake-up point.
- **Bounded work.** Keep everything done under `m_mutex` short and of fixed size. There must be no loop whose length depends on host input.

### Protocol conventions

Each protocol-facing manager defines its own messages on top of the `uart_comm` frame. All of them follow these rules:
- **Version byte first.** Every payload starts with `kProtocolVersion`. Any change to the wire format bumps it. No backward compatibility is kept: the host and the firmware are updated together.
- **Request correlation.** Configure commands carry a host-chosen `seq` byte right after the version, and the response echoes it. A response to a header that could not be parsed uses `0xFF` (`kUnparsedHeaderByte`) for the subcommand and for the id fields. ESP-initiated requests carry their own id, like time_sync's `u16 request_id`.
- **Encoding.** All multi-byte fields are little-endian. Floats are IEEE-754 binary32. Encode and decode field by field with the bounds-checked little-endian `uart_comm::wire::Reader` / `Writer` (`uart_comm/include/wire_codec.hpp`). Never `memcpy` a whole struct to or from the wire, and never write a component-local byte-order helper.
- **Exact lengths.** A fixed-size body must be exactly its size, or it is rejected. A variable-size body carries an explicit count, and `count × entry size` must equal the remaining length.
- **Untrusted input.** Reject invalid host input with an error result code and apply nothing. Invalid means a wrong length, an unknown subcommand or id, a non-finite float, a GPIO outside the allowed set, or an invalid enum value. A valid value outside its range is clamped, and the response reports the value actually applied (as `SET_TIMING` does).
- **IDs from Kconfig.** MSG_TYPE values and stream channels come only from Kconfig, within the `uart_comm` ranges above.
- **The README is the spec.** The component README, or `docs/protocol-api-guide.md` for the IMU, defines the wire format. It is updated in the same commit as any wire change.

### Errors, failures and memory

- Check every ESP-IDF return value, and log failures with `esp_err_to_name`.
- **Report errors to the host through diag** (see `components/diag/README.md`):
  - Managers and `main` use `DIAG_ERROR` / `DIAG_WARN` for errors and warnings the host should know about. These write the console line and queue a code.
  - Frequent faults are counted with `DIAG_COUNT`. It is the only diag macro allowed on real-time paths.
  - Each new code goes in `diag_codes.yaml`, then regenerate.
  - Plain `ESP_LOGx` is for info, for programmer errors, and for host commands rejected with a result code (the response already tells the host).
  - A manager constructor that fails reports `<source>.InitFailed` with the failing stage in `arg0`. Recoverable conditions (a stalled timer, failing syncs) are reported once when they start and `DIAG_CLEAR`ed on recovery or when the task watching them stops, not on every occurrence. A real-time task only counts; a non-real-time task watches the count and reports (the motor config task does this for the control timer).
  - `CONFIG_DIAG_CONSOLE_LOG=n` compiles out all first-party console text (the root `CMakeLists.txt` sets `LOG_LOCAL_LEVEL=ESP_LOG_NONE` on `components/` and `main/`). Code must build warning-free that way too, which CI checks.
- **Never abort on runtime errors** that the host or the hardware can cause. Return `esp_err_t` or `bool`, expose `isValid()`, and log the failure.
  - No `ESP_ERROR_CHECK` anywhere.
  - `assert` / `configASSERT` / `abort` are only for invariants that indicate a programmer error. One example: `UartComm` being destroyed while registrations are still active.
- **Exceptions and RTTI are disabled** (sdkconfig).
- **Allocate only at setup or configure time:** constructors, `start()`, and create/configure commands. Never allocate on steady-state paths. Size containers up front (`reserve` / `resize`).
  - Running out of memory is treated as fatal. Plain `new` / `std::make_unique`, which abort on failure, are acceptable.

### Definition of done

A change is done when all of the following are true:
- **Builds:** `idf.py build` (also with `sdkconfig.ci.nolog`) and `idf.py -C test/uart_comm build` succeed with zero warnings, the generated diag files are current, and the host tests pass. CI checks all of them.
- **Docs in the same commit:**
  - Any public API, protocol or Kconfig change updates the component README in that commit.
  - A change to an architecture, concurrency or convention rule updates this file or `components/CLAUDE.md`.
- **Review status:** a change that fixes or affects a `review/` item marks its status in the same change: **Fixed**, **Partly fixed** or **No longer applicable**, with a one-line note.
- **Host tests for pure logic:** new or changed pure-logic code (PID, filters, estimators, protocol encode/decode) comes with host unit tests in `test/host/`, and they pass.

### Files not to edit

- **Generated files:** don't edit anything under `build/` or the generated `sdkconfig` by hand. `components/diag/include/diag_codes.hpp`, `components/diag/host/diag_codes.py` and the code table in `components/diag/README.md` come from `diag_codes.yaml` through `tools/gen_diag_codes.py`.
- **Vendor code:**
  - `components/imu_bmo_055/vendor/bno055.c` / `bno055.h` are the Bosch driver. `vendor/` is a private include dir; only `Bno055Device` uses it.
- **Naming:** the component and namespace are spelled `imu_bmo_055`, but the part is the BNO055.

## Review notes and TODO

- `review/` holds a full-repo code review, with one markdown file per aspect and a ranked index in `review/README.md`. Check it before changing an area: many known issues are listed there with fixes, and each item's status is marked.
- `TODO.md` lists planned work that is not started yet, including the fixes for current convention violations.
