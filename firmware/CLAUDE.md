# CLAUDE.md (firmware)

This file provides guidance to Claude Code (claude.ai/code) when working on the firmware. The repo map and cross-cutting rules are in the root [CLAUDE.md](../CLAUDE.md).

**Paths.** Every path in this file is relative to the repository root, so the firmware sources are `firmware/main/` and `firmware/components/`. Commands run from the repository root and use `-C firmware`.

## What this is

DriveLink v2 is ESP-IDF 5.5.2 firmware for the ESP32-S3, written in C++. It drives DC motors (MCPWM + DIR GPIO), reads quadrature encoders (PCNT), runs PID velocity/position loops, streams BNO055 IMU data, and syncs time with a host. The host talks to the board over a binary UART protocol on UART0 (TX=GPIO43, RX=GPIO44). The console runs on USB-Serial-JTAG, so logs do not mix with protocol traffic.

## Build, flash, test

ESP-IDF must be exported in the shell first:
- PowerShell: `. $HOME\esp\v5.5.2\esp-idf\export.ps1`
- On this machine, the IDF Python env is built for Python 3.11. If `export.ps1` reports a missing `py3.14` env, put `C:\Espressif\tools\idf-python\3.11.2` first on `PATH` and run export again.
- The shell's `IDF_TOOLS_PATH` points at `~\.espressif\Espressif`, which has no `python_env`. `export.ps1` then fails ("ESP-IDF Python virtual environment ... not found"), and `idf.py` is not found. The env is under `~\.espressif`. Shell state does not persist between agent tool calls, so run everything in one PowerShell call:
  ```powershell
  $env:IDF_TOOLS_PATH = "$HOME\.espressif"; $env:PATH = "C:\Espressif\tools\idf-python\3.11.2;" + $env:PATH; . $HOME\esp\v5.5.2\esp-idf\export.ps1 *> $null; idf.py -C firmware build
  ```

```bash
idf.py -C firmware build                 # firmware (target esp32s3, from sdkconfig.defaults)
idf.py -C firmware build flash monitor
idf.py -C firmware menuconfig            # all component tunables live under their Kconfig menus
idf.py -C firmware fullclean             # when target/config changes leave the build inconsistent
```

There are two kinds of automated tests.

**Native unit tests** (`firmware/test/native/`) cover everything that builds without ESP-IDF:
- the `protocol/` library (CRC, framing, scanner, wire codec, the generated definitions; its tests live in `protocol/test/` and are compiled into this build)
- `core_os`'s native backend (with the in-memory KV store and its fault injection: power loss, failed and torn writes) and the pure `core` (registry, bind API, values, loops, param batches, the pending layout and its validation, the wiring order, the NVS blobs and A/B persistence, safe mode, the boot, a threaded end-to-end run). The generator's fixture types (`fixture.full`, `fixture.sensor`, and the wiring set `actuator.fixture`, `feedback.fixture`, `fixture.wired`) serve as test module types (`tests/core_test_modules.hpp`). Core tests build a layout by writing it to a native `KvStore` (`writeLayout`) and booting a `Core` on it (`bootWith`); a reboot is a new `Core` on the same store.
- the legacy motor_manager / IMU / time_sync / diag protocol codecs, diag's slot table, the PID and the velocity estimator

It is a plain CMake project (target `native_tests`) with a vendored doctest (`firmware/test/native/third_party/doctest`). It builds `protocol/` as the host library `drivelink::protocol`, and compiles `core_os/backend/native`, the `core` sources (all but `src/idf/`) and the legacy sources directly with the host compiler. `firmware/test/native/shim/` stands in for the two IDF headers the legacy sources include (`sdkconfig.h` with the Kconfig defaults they read, and a no-op `esp_log.h`); the core needs neither. No IDF export is needed; on this machine, Strawberry Perl ships `g++` (MinGW-w64, posix threads), `cmake` and `ninja`. The build also needs Python 3 with PyYAML: it runs the generator on the fixtures in `tools/gen/tests/fixtures` to make the type headers the tests include.

```bash
cmake -S firmware/test/native -B firmware/test/native/build -G Ninja
cmake --build firmware/test/native/build
ctest --test-dir firmware/test/native/build --output-on-failure   # or run firmware/test/native/build/native_tests directly
firmware/test/native/build/native_tests -tc="*SetTiming*"          # doctest filter
```

A legacy source can join the native build only if it builds without ESP-IDF beyond those shims. Keep codecs and math in their own files (`*_protocol.cpp`, `velocity_estimator.cpp`), apart from the code that touches FreeRTOS or drivers, and add them to `FIRMWARE_SOURCES` in `firmware/test/native/CMakeLists.txt`. If one reads a new `CONFIG_*` symbol, add it to `firmware/test/native/shim/sdkconfig.h`. A new `core` source goes in `CORE_SOURCES` there, and must not need the shims. Tests that use threads (the `core_os` timing and the threaded core run) keep loose timing bounds: a desktop scheduler can wake a thread 15 ms late.

**CI** (`.github/workflows/ci.yml`) runs on every pull request and on pushes to `main` and `firmware_overhaul`:
- `idf.py build` in the `espressif/idf:v5.5.2` container, which fails on any compiler warning. It builds `firmware/`, then `firmware/` again with `firmware/sdkconfig.ci.nolog` (`CONFIG_DIAG_CONSOLE_LOG=n`), then `firmware/test/uart_comm`.
- On Ubuntu, `tools/gen/gen.py --check`, the generator's unit tests (`python3 -m unittest discover -s tools/gen/tests`) and the native tests.

**Definitions** are in `protocol/defs/`: `registry.yaml` (MSG_TYPEs and stream channel ranges), `diag_codes.yaml` (the diag codes), `system.yaml` (the system items, type `0x00`: the loop rates so far) and, from F3b, one file per module type. After editing any of them, regenerate and commit the outputs; CI fails if they are stale. The generator needs PyYAML, which the IDF Python env and this machine's Python both have. The files and the schema are described in `docs/protocol/definitions.md`.

```bash
python tools/gen/gen.py            # regenerate
python tools/gen/gen.py --check    # what CI runs
python -m unittest discover -s tools/gen/tests
```

It writes the headers in `protocol/include/drivelink/protocol/gen/` (including the descriptor hash) and the generated tables in `docs/protocol/`. `firmware/components/diag/include/diag_codes.hpp` is a hand-written shim over the generated diag table, so call sites keep writing `diag::codes::<source>::k<Name>`.

**Hardware-in-the-loop tests** for `uart_comm` live in `firmware/test/uart_comm/`, a separate IDF app whose `EXTRA_COMPONENT_DIRS` points at `../../components` and that builds only the components `main` requires (`set(COMPONENTS main)`).

```bash
idf.py -C firmware/test/uart_comm build flash           # flash the test firmware
cd firmware/test/uart_comm && pip install -r requirements.txt
pytest pytest_uart_comm.py --port COM5                      # USB-UART adapter on GPIO43/44
pytest pytest_uart_comm.py --port COM5 -k test_echo_max_payload   # single test
```

`conftest.py` defaults `--baud` to 921600, the Kconfig default. The same directory holds `smoke_main_firmware.py`, a standalone script for the **main** firmware (not the test app): it checks that diag, motor_manager, the IMU and time_sync answer over the link with their current protocol versions, and that the IMU stream arrives on its registry channel (`python smoke_main_firmware.py --port COM5`). It creates no motor, so nothing can move.

**Stack sizes** are checked on hardware. The main firmware logs each of its tasks' stack high-water mark on the console every `CONFIG_DRIVELINK_STACK_LOG_PERIOD_S` (30 s, 0 = off; the task names are listed in `firmware/main/main.cpp`, so add a new task there). It uses `uxTaskGetStackHighWaterMark()`, which takes no kernel lock; don't use `uxTaskGetSystemState()`, which scans every stack inside a critical section and stalls the control loop on the other core. The HIL test app logs the uart_comm RX/TX marks when idle.

**Changed Kconfig defaults don't reach an existing `sdkconfig`.** After a pull that changes a default (F1b changed the baud rate, `MAX_PAYLOAD`, the main task stack and the motor config task stack), delete `firmware/sdkconfig` and `firmware/test/uart_comm/sdkconfig` before building, or the old values stay. F2 removed the MSG_TYPE and stream channel options; an old `sdkconfig` still lists them, harmlessly, until you delete it.

Everything else is checked by a clean `idf.py -C firmware build` (there are no warnings today, so keep it that way) and by watching runtime logs.

## Architecture

**Startup** (`firmware/main/main.cpp`):
- The order is `diag::init()`, `UartComm`, then `MotorManager`, `TimeSync` and `ImuBno055Manager` (each takes a reference to `UartComm`), then the core (`startCore()`), then `diag::Reporter`.
- Each manager is checked with `isValid()`. One that failed is reported (`system.ManagerStartFailed`), destroyed through its `std::optional`, and left out, so destructors must be safe at any point. The rest keep running.
- Before the core, main initialises NVS (`core_os::nvsInit()`): a full partition or one from a newer NVS version is erased and initialised again, and both that and a failure are reported (`core.NvsInitFailed`, arg0 1 erased, 2 failed). The core boots either way.
- The core is built on the heap (`app_main`'s stack is small) from `coreConfigFromKconfig()` and main's module list (`kModuleTypes`, empty until F3b; each desc is checked against `defs::kTypeTable`). main calls `init()`, then `boot(store)` (safe-mode check, NVS load, layout validation, creation in dependency order, tick order; the `KvStore` is a static in main), then `start()`. If one fails, main reports `core.InitFailed` (arg0 the stage: 1 registry, 2 control loop task, 3 sensor loop task, 4 boot) and continues without the core. It logs `Core started: ...` with the running, failed and configured instance counts, safe mode, whether the config came from NVS, and the loop periods.
- `CONFIG_CORE_BOOT_SELFTEST` (main's Kconfig, default off, bench only, never committed on) flips `control.rate_hz` between 1000 and 500 and SAVEs on every boot, to check SAVE and the reload on hardware.
- The Reporter comes last so its `Boot` event carries the final `started_mask`. Records from the startup wait in the diag queue until then.
- `app_main` never returns. If `UartComm` fails, there is no link to report on: it logs to the console and idles.

**Layering** (each one is an ESP-IDF component under `firmware/components/`):
- `motor_hal` (`motor_hal::Motor`) and `rotary_encoder_hal` (`rotary_encoder_hal::Encoder`) are move-only hardware wrappers. Scarce peripherals come from static pools (`McpwmPool`, `PcntPool`) guarded by a FreeRTOS mutex.
- `pid` is pure math.
- `motor_controller` combines one motor, one encoder and the PIDs into a controller with open-loop, velocity and position modes. Its velocity estimate is the pure `VelocityEstimator`.
- `motor_manager`, `imu_bmo_055` and `time_sync` are the protocol-facing managers. Each registers UART callbacks and owns its own tasks. Each keeps its payload codec in a pure module with no ESP-IDF dependency (`motor_protocol`, `imu_protocol`, `time_sync_protocol`, all private), built on `drivelink::protocol::wire::Reader` / `Writer`. The managers decode and encode only through these, so the native tests cover the wire format.
- `protocol/` (at the repo root, an IDF component through `EXTRA_COMPONENT_DIRS`) holds the pure wire library: CRC, framing, the frame `Scanner` and `wire_codec`, plus the generated registry, diag code table and descriptor format (`protocol/defs/` is their source). `uart_comm` and every protocol codec build on it. Its rules are in [protocol/CLAUDE.md](../protocol/CLAUDE.md).
- `rtos_util` provides `StoppableTask`, `TaskTimer` and `msToTicksCeil`.
- `core_os` holds thin OS adapters (clock, `Mutex`, `Event`, `PeriodicTask`, `OneShotTimer`, the diag sink, the `KvStore` on NVS, the boot info: reset reason, RTC crash counter, reboot) with an ESP-IDF backend (built on `rtos_util`) and a native one. The backend is picked by include dir, never by `#ifdef`. See `firmware/components/core_os/README.md`.
- `core` is the new core (F3 onwards): the module registry, the instance bind API, the `control` and `sensor` loops, the param service, the layout with its pending copy and validation, NVS A/B persistence, safe mode and the boot. It is pure: it uses only `core_os`, `protocol/` and std, so it also builds natively. Kconfig reaches it through `CoreConfig`, filled by its one IDF-only source. See `firmware/components/core/README.md`. Until the legacy managers are ported, it runs beside them with empty loops.
- `diag` reports errors to the host as codes. The `DIAG_*` macros latch a per-code slot and queue a record, and `diag::Reporter` sends events and answers queries over `uart_comm`. Only the managers and `main` report. The hardware layers return errors and don't depend on `diag`, and the reporter reads `uart_comm`'s link counters itself. Its codec and slot table are pure modules (`diag_protocol`, `diag_table`).

**UART protocol** (`uart_comm`):
- Frame layout: `AA AA AA | MSG_TYPE | LEN u16 | HCRC | payload | CRC16`, specified in `docs/protocol/framing.md`. HCRC is a CRC-8 over MSG_TYPE and LEN; the CRC16 (CCITT-FALSE) covers MSG_TYPE, LEN, HCRC and the payload. Encoding, CRCs and scanning come from `protocol/`.
- `MAX_PAYLOAD` is 1024 (`CONFIG_UART_COMM_MAX_PAYLOAD`, 128 to 1024; the protocol limit is `drivelink::protocol::kMaxPayload`). It sizes `RxFrame`, so every queue of `RxFrame`s and every `RxFrame` on a stack costs about 1 KB per entry. Don't put an `RxFrame` or a `MAX_PAYLOAD` buffer on a task stack without checking that task's stack mark.
- The default baud rate is 921600, and the UART ISR runs from IRAM (`CONFIG_UART_ISR_IN_IRAM`). The driver's RX and TX ring buffers are 4096 bytes, above one full frame.
- RX framing is done in software; hardware pattern detection is not used. `uart_comm`'s `Parser` is the driver side: driver events only wake the RX task, and `Parser::nextFrame()` pulls bytes from the ring buffer into a `drivelink::protocol::Scanner`, which scans for the preamble. The preamble is not escaped, so a rejected candidate (bad HCRC, LEN or CRC16) drops one byte and rescans. The HCRC and LEN are checked before the payload is awaited, so a false preamble doesn't hold up RX. Never flush the RX buffer: it may already hold the next valid frames. The stale-frame timer and the logging stay in `Parser`; the scanner has no clock.
- MSG_TYPE ranges: commands `0x01–0x3F`, responses `0x40–0x7F`, streams `0x80–0xBF`, events `0xC0–0xFE`.
- TX has two paths, and both use only buffers allocated in the constructor or `openStream()`:
  - `sendCommand()` takes a free frame buffer from a preallocated pool (`CONFIG_UART_COMM_CMD_QUEUE_DEPTH` buffers), encodes into it and queues its index. The TX task drains the queue first. `ESP_ERR_NO_MEM` means the pool is empty.
  - `sendData()` writes to a per-channel stream slot where only the latest value is sent. A channel must be opened once at setup with `openStream(channel, max_len)`, which allocates the slot (two buffers of `max_len`). `sendData()` never blocks or allocates: it try-locks the slot, copies into the buffer the TX task isn't sending, and returns `ESP_ERR_TIMEOUT` if the slot was busy (counted in `ErrorCounters::stream_skipped`). Call it freely from the control tick.
  - `UartComm::stackHighWater()` reports the RX and TX tasks' free stack, for logging. The HIL test app logs it when idle.
- RX either calls a callback registered for the MSG_TYPE, on the RX task, or pushes the frame to a queue read by `receive()`.
- Message IDs and stream channels come from the generated registry (`protocol/defs/registry.yaml`, listed in `docs/protocol/registry.md`), through `<drivelink/protocol/gen/registry.hpp>`: `drivelink::protocol::defs::msg::k<Name>`.
  - The generator and the header's `static_assert`s check that values are unique, in their class range, and clear of the group channel ranges.
  - The legacy managers take their defaults from it: time_sync `0x20` / `0x60`, motor_manager `0x21` / `0x61` with the setpoint stream on `0xBD` and telemetry on `0xBE`, the IMU `0x22` / `0x42` with its stream on `0xBF`. The streams sit at the top of the command group range until F5b and F6 remove them.
  - diag keeps query `0x23` / `0x63` and events `0xC0`.
- Every protocol payload starts with a version byte (`kProtocolVersion`).
  - motor_manager and IMU are both at v6 (v6 changed no payload: it was bumped with the F2 move of their stream channels, and v5 with the F1b framing). Both have their configure commands and responses carry a host-chosen `seq` byte right after the version, echoed in the response. Every configure response carries `result` then a `detail` byte (each manager's `ResultDetail`) saying why it failed; the manager methods return the pair as a `Result`. A command that arrives while the config queue is full is answered from the RX callback (`QueueFull`) instead of being dropped. A response to a header that could not be parsed uses `0xFF` (`kUnparsedHeaderByte`) for subcommand (and motor_id). Both require exact configure body sizes.
  - motor_manager: `SET_TIMING` (`0x0B`) sets the control and telemetry periods; its response appends the applied values after the 6-byte header.
  - IMU: `SetStreamTiming` (`0x02`) sets the sample and telemetry periods; its response appends the applied values after the 5-byte header.
  - Motor and IMU stream timestamps are both wall-clock µs (`gettimeofday`, corrected by time_sync), latched at the sampling instant.
  - time_sync is at v2, diag at v2 (both bumped with the F1b framing). time_sync's ESP-initiated request already carries a `u16 request_id`.

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
- **Core loops:**
  - `control` (`core_ctrl`: core 1, priority 20, default 1 kHz) and `sensor` (`core_sens`: core 0, priority 10, default 100 Hz) each run on a `core_os::PeriodicTask`, a `TaskTimer`-woken `StoppableTask` on the device. The rates are the system params `control.rate_hz` and `sensor.rate_hz`; a change restarts the loop's timer from the caller of the param service.
  - Each tick: heartbeat, then the staged param batch, then each instance's `tick()` with `dt = elapsed × period`, then the stats. Instances tick in the wiring's tick order, fixed at boot: a ref's target ticks first, unless the ref is `ref_order: after` (feedback → controller → actuator). Creation is always dependencies first.
  - Param batches have one domain: system (on the caller), one loop (staged), offline (an instance that isn't running: its shadow, on the caller) or pending (`reboot_required` params and instances that exist only in the pending layout; answers `PendingReboot`).
  - Param batches reach a loop only through its one staging slot, an atomic state (Empty → Filling → Ready → Applying → Applied) that the config task and the loop move with stores and compare-and-swaps. The config task waits on a `core_os::Event` for up to 3 periods plus 5 ms, then withdraws the batch (Ready → Empty). The loop never waits for the config task.
  - The config task reads and returns param values from its own shadow copy, never from the storage the loop owns.

## Conventions

This file holds the firmware-wide conventions. Component rules (layout, Kconfig scope, API shape, resources, logging, naming and style) are in [firmware/components/CLAUDE.md](components/CLAUDE.md).

### Real-time paths

These paths are real-time:
- the motor control tick (`MotorManager::updateTaskLoop` work, `MotorController::update`, `PidController::compute`)
- the core loop tick (`core::Loop::runTick`, every `Instance::tick` in the `control` loop), the staging swap inside it, and the params' `OnChange` hooks
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
- **Encoding.** All multi-byte fields are little-endian. Floats are IEEE-754 binary32. Encode and decode field by field with the bounds-checked little-endian `drivelink::protocol::wire::Reader` / `Writer` (`protocol/include/drivelink/protocol/wire_codec.hpp`). Never `memcpy` a whole struct to or from the wire, and never write a component-local byte-order helper.
- **Exact lengths.** A fixed-size body must be exactly its size, or it is rejected. A variable-size body carries an explicit count, and `count × entry size` must equal the remaining length.
- **Result and detail.** A configure response carries `result` and then a `detail` byte from the manager's `ResultDetail` enum, which names the check that failed (`None` on success). Manager methods return the pair as a `Result`, so each failure site sets its own detail. Detail values are part of the wire format: add new ones, never renumber. Every command gets a response, including one dropped because the config queue is full.
- **Untrusted input.** Reject invalid host input with an error result code and apply nothing. Invalid means a wrong length, an unknown subcommand or id, a non-finite float, a GPIO outside the allowed set, or an invalid enum value. A valid value outside its range is clamped, and the response reports the value actually applied (as `SET_TIMING` does).
- **IDs from the registry.** MSG_TYPE values and stream channels come only from `protocol/defs/registry.yaml`, through the generated `gen/registry.hpp`. Never hard-code one, and never add a Kconfig option for one.
- **`docs/protocol/` is the spec.** It defines the wire format, and is updated in the same commit as any wire change. The legacy managers are the exception during the overhaul (see the overhaul section in the root `CLAUDE.md`): their wire format is their code and native tests, and the IMU also has `firmware/components/imu_bmo_055/docs/protocol-api-guide.md`.

### Errors, failures and memory

- Check every ESP-IDF return value, and log failures with `esp_err_to_name`.
- **Report errors to the host through diag** (the codes are listed in `docs/protocol/diag-codes.md`):
  - Managers and `main` use `DIAG_ERROR` / `DIAG_WARN` for errors and warnings the host should know about. These write the console line and queue a code.
  - Frequent faults are counted with `DIAG_COUNT`. It is the only diag macro allowed on real-time paths.
  - Each new code goes in `protocol/defs/diag_codes.yaml`, then regenerate.
  - Plain `ESP_LOGx` is for info, for programmer errors, and for host commands rejected with a result code (the response already tells the host).
  - A manager constructor that fails reports `<source>.InitFailed` with the failing stage in `arg0`. Recoverable conditions (a stalled timer, failing syncs) are reported once when they start and `DIAG_CLEAR`ed on recovery or when the task watching them stops, not on every occurrence. A real-time task only counts; a non-real-time task watches the count and reports (the motor config task does this for the control timer).
  - `CONFIG_DIAG_CONSOLE_LOG=n` compiles out all first-party console text (`firmware/CMakeLists.txt` sets `LOG_LOCAL_LEVEL=ESP_LOG_NONE` on `firmware/components/` and `firmware/main/`). Code must build warning-free that way too, which CI checks.
- **Never abort on runtime errors** that the host or the hardware can cause. Return `esp_err_t` or `bool`, expose `isValid()`, and log the failure.
  - No `ESP_ERROR_CHECK` anywhere.
  - `assert` / `configASSERT` / `abort` are only for invariants that indicate a programmer error. One example: `UartComm` being destroyed while registrations are still active.
- **Exceptions and RTTI are disabled** (sdkconfig).
- **Allocate only at setup or configure time:** constructors, `start()`, and create/configure commands. Never allocate on steady-state paths. Size containers up front (`reserve` / `resize`).
  - Running out of memory is treated as fatal. Plain `new` / `std::make_unique`, which abort on failure, are acceptable.
- **NVS** belongs to the core (`core_os::KvStore`, namespaces `core` and `t<type>i<inst>`; see `firmware/components/core/README.md`). An NVS write stalls the caches of both cores for milliseconds, so it never runs on a real-time path, and SAVE and FACTORY_RESET are refused while armed. The legacy managers don't use NVS.

### Definition of done

A change is done when all of the following are true:
- **Builds:** `idf.py -C firmware build` (also with `firmware/sdkconfig.ci.nolog`) and `idf.py -C firmware/test/uart_comm build` succeed with zero warnings, the generated files are current (`python tools/gen/gen.py --check`), and the native tests and the generator's unit tests pass. CI checks all of them.
- **Docs in the same commit:**
  - Any protocol change updates `docs/protocol/` in that commit. A public API or Kconfig change updates the component's README, if it has one.
  - A change to an architecture, concurrency or convention rule updates this file, `firmware/components/CLAUDE.md`, or the root `CLAUDE.md` for a cross-cutting rule.
- **Overhaul status:** during the overhaul, every PR updates `plans/overhaul-status.md` (see the root `CLAUDE.md`).
- **Native tests for pure logic:** new or changed pure-logic code (the core, PID, filters, estimators, protocol encode/decode) comes with native unit tests (`firmware/test/native/`, or `protocol/test/` for the `protocol/` library), and they pass.

### Files not to edit

- **Generated files:** don't edit anything under `firmware/build/` (or another build dir such as `build_variant/`) or the generated `sdkconfig` by hand. Everything under `protocol/include/drivelink/protocol/gen/` and the generated tables in `docs/protocol/` (`registry.md`, `diag-codes.md`, `types/`) come from `protocol/defs/` through `tools/gen/gen.py`. Edit the YAML and regenerate.
- **Vendor code:**
  - `firmware/components/imu_bmo_055/vendor/bno055.c` / `bno055.h` are the Bosch driver. `vendor/` is a private include dir; only `Bno055Device` uses it.
- **Naming:** the component and namespace are spelled `imu_bmo_055`, but the part is the BNO055.
