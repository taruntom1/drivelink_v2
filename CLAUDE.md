# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DriveLink v2 is ESP-IDF 5.5.2 firmware for the ESP32-S3, written in C++. It drives DC motors (MCPWM + DIR GPIO), reads quadrature encoders (PCNT), runs PID velocity/position loops, streams BNO055 IMU data, and syncs time with a host. The host talks to the board over a binary UART protocol on UART0 (TX=GPIO43, RX=GPIO44). The console runs on USB-Serial-JTAG, so logs do not mix with protocol traffic.

## Build, flash, test

ESP-IDF must be exported in the shell first:
- PowerShell: `. $HOME\esp\v5.5.2\esp-idf\export.ps1`
- On this machine, the IDF Python env is built for Python 3.11. If `export.ps1` reports a missing `py3.14` env, put `C:\Espressif\tools\idf-python\3.11.2` first on `PATH` and run export again.

```bash
idf.py build                 # firmware (target esp32s3, from sdkconfig.defaults)
idf.py build flash monitor
idf.py menuconfig            # all component tunables live under their Kconfig menus
idf.py fullclean             # when target/config changes leave the build inconsistent
```

The only automated tests are hardware-in-the-loop tests for `uart_comm`. They live in `test/uart_comm/`, a separate IDF app whose `EXTRA_COMPONENT_DIRS` points at `../../components`.

```bash
idf.py -C test/uart_comm build flash           # flash the test firmware
cd test/uart_comm && pip install -r requirements.txt
pytest pytest_uart_comm.py --port COM5                      # USB-UART adapter on GPIO43/44
pytest pytest_uart_comm.py --port COM5 -k test_echo_max_payload   # single test
```

`conftest.py` defaults `--baud` to 115200, which matches the Kconfig default, even though some comments say 921600. Everything else is checked by a clean `idf.py build` (there are no warnings today, so keep it that way) and by watching runtime logs.

## Architecture

**Startup** (`main/main.cpp`): `UartComm` comes first. `MotorManager`, `TimeSync` and `ImuBno055Manager` then each take a reference to it. Each one is checked with `isValid()`. An early `return` destroys the objects in reverse order, so destructors must be safe at any point.

**Layering** (each one is an ESP-IDF component under `components/`):
- `motor_hal` (`esp_motor::Motor`) and `rotary_encoder_hal` (`esp_rotary_encoder::Encoder`) are move-only hardware wrappers. Scarce peripherals come from static pools (`McpwmPool`, `PcntPool`) guarded by a FreeRTOS mutex.
- `pid` is pure math.
- `motor_controller` combines one motor, one encoder and the PIDs into a controller with open-loop, velocity and position modes.
- `motor_manager`, `imu_bmo_055` and `time_sync` are the protocol-facing managers. Each registers UART callbacks and owns its own tasks.
- `rtos_util` provides `StoppableTask`.

**UART protocol** (`uart_comm`):
- Frame layout: `AA AA AA | MSG_TYPE | LEN | payload | CRC16-CCITT(0x1021, init 0xFFFF)`. The CRC covers MSG_TYPE, LEN and the payload. RX finds frame starts with the UART hardware's pattern detection.
- MSG_TYPE ranges: commands `0x01–0x3F`, responses `0x40–0x7F`, streams `0x80–0xBF`, events `0xC0–0xFE`.
- TX has two paths. `sendCommand()` uses a FIFO queue that is drained first. `sendData()` writes to per-channel stream slots where only the latest value is sent.
- RX either calls a callback registered for the MSG_TYPE, on the RX task, or pushes the frame to a queue read by `receive()`.
- Message IDs and channels come from each component's Kconfig:
  - time_sync: `0x20` / `0x60`
  - motor_manager: `0x21` / `0x61`, setpoint stream `0x80`, telemetry `0x81`
  - IMU: `0x22` / `0x42`, stream `0x82`
  - Nothing checks for collisions between these IDs.
- Every protocol payload starts with a version byte (`kProtocolVersion`).

**Concurrency rules**:
- **Task lifecycle:** create every task with `rtos_util::StoppableTask`. Never use bare `xTaskCreate` / `vTaskDelete`.
  - Task bodies loop on `stopRequested()` and sleep with `sleepFor` / `sleepUntil`.
  - Every other blocking wait needs a bounded timeout.
  - Owners call `stop()` in their destructor, which waits for the body to return, before freeing anything the task uses.
- **UART callbacks:** `registerCallback(type, cb, registration)` fills in a `uart_comm::CallbackRegistration`, which unregisters on destruction. Unregistering waits for a callback that is already running.
  - Destructors reset their registrations **first**.
  - Registrations must not outlive the `UartComm` (its destructor aborts if one is still active).
  - Callbacks run on the RX task, so they must not block. Hand work off through a queue instead: managers forward configure commands to their own config task queue.
  - Setpoint streams go into a mailbox that the motor update task applies on its next tick.
- **Wake-up frames:** managers wake their config task with a dummy queue item on shutdown. The config task must re-check the stop flag before dispatching anything it receives.

## Conventions (from `.github/copilot-instructions.md` and `.github/instructions/hal-component-patterns.instructions.md`)

- **Component layout:**
  - `include/` holds public headers, `priv_include/` internal ones, `src/` the implementation.
  - `Kconfig` holds tunable defaults and their ranges.
  - `CMakeLists.txt` uses `idf_component_register` with explicit `SRCS` / `INCLUDE_DIRS` / `PRIV_INCLUDE_DIRS` / `REQUIRES`.
- **Hardware-owning types:** non-copyable, movable only when transferring ownership is safe, low-level state hidden in an Impl or pool, and an `isValid()` check whenever construction can fail.
- **Errors and logging:** check every ESP-IDF return value, log failures with `esp_err_to_name`, use one module-level `TAG` per file, and include pins, channels and slots in log messages. Fail early and leave the object in a consistent state.
- **Numeric inputs:** clamp user-facing numbers at the API boundary. Convenience setters go through one canonical path. Stop and reset are explicit and idempotent.
- **Generated files:** don't edit anything under `build/` or the generated `sdkconfig` by hand.
- **Vendor code:** `components/imu_bmo_055/bno055.c` / `bno055.h` are the Bosch driver. `bno055_support.c` is Bosch's example template. It is compiled in, but none of our code calls it.
- **Naming:** the component and namespace are spelled `imu_bmo_055`, but the part is the BNO055.

## Review notes

`review/` holds a full-repo code review, with one markdown file per aspect and a ranked index in `review/README.md`. Check it before changing an area: many known issues are listed there with fixes, and each item's status is marked.
