# CLAUDE.md (firmware)

Guidance for working on the firmware. The repo map and the cross-cutting rules are in the root [CLAUDE.md](../CLAUDE.md).

**Paths.** Every path in this file is relative to the repository root, so the firmware sources are `firmware/main/` and `firmware/components/`. Commands run from the repository root and use `-C firmware`.

**Keep this file short.** It holds what applies to all firmware work. Detail about one area goes in that area's file (the table below), with a link from here.

## What this is

ESP-IDF 5.5.2 firmware for the ESP32-S3, written in C++. It drives DC motors (MCPWM + DIR GPIO), reads quadrature encoders (PCNT), runs PID velocity and position loops, streams BNO055 IMU data, and syncs time with a host. The host talks to the board over a binary UART protocol on UART0 (TX=GPIO43, RX=GPIO44). The console runs on USB-Serial-JTAG, so logs do not mix with protocol traffic.

## Where the details are

A `CLAUDE.md` in a directory loads by itself when you work there. Open the other files before you change their area.

| Area | Read |
|---|---|
| Any component: layout, Kconfig scope, API shape, resources, logging, naming | [firmware/components/CLAUDE.md](components/CLAUDE.md) |
| `main.cpp`: startup order, the stack log, bench-only options | [firmware/main/CLAUDE.md](main/CLAUDE.md) |
| Tests: what the native tests cover, adding a source to them, HIL tests, smoke scripts, CI | [firmware/test/CLAUDE.md](test/CLAUDE.md) |
| `uart_comm` internals: RX framing, the TX paths, limits | [firmware/components/uart_comm/CLAUDE.md](components/uart_comm/CLAUDE.md) |
| The core: boot, loops, layout, params, NVS | [core/README.md](components/core/README.md), [core_os/README.md](components/core_os/README.md), [mod_mock/README.md](components/mod_mock/README.md) |
| `motor_manager`, `imu_bmo_055`, `time_sync`: their loops and wire format | [firmware/components/legacy-managers.md](components/legacy-managers.md) |
| The wire format | `docs/protocol/` (`framing.md`, `core.md`, `diag.md`, `registry.md`, `diag-codes.md`) and [protocol/CLAUDE.md](../protocol/CLAUDE.md) |
| The definition YAMLs and the generator | `docs/protocol/definitions.md` |

## Commands

**All the checks, one command** (from the repository root, in any shell; it exports ESP-IDF itself):

```bash
python tools/validate.py quick             # generator, native tests, this OS's host tests; no IDF
python tools/validate.py full              # every check; run it before a PR
python tools/validate.py --only fw-nolog   # one check (--list names them)
```

It runs the checks one at a time, skips a check whose code is unchanged since it passed, and writes the logs and a summary to `build/validate/`. The guide is in [firmware/test/CLAUDE.md](test/CLAUDE.md) ("Validation"). The commands below are the ones it runs, for working on one build by hand.

**Export ESP-IDF first** (for `idf.py` by hand; `tools/validate.py` does this itself). In PowerShell: `. $HOME\esp\v5.5.2\esp-idf\export.ps1`. On this machine two things go wrong:
- The IDF Python env is built for Python 3.11. If `export.ps1` reports a missing `py3.14` env, put `C:\Espressif\tools\idf-python\3.11.2` first on `PATH` and export again.
- The shell's `IDF_TOOLS_PATH` points at `~\.espressif\Espressif`, which has no `python_env` (the env is under `~\.espressif`). `export.ps1` then fails with "ESP-IDF Python virtual environment ... not found", and `idf.py` is not found.

Shell state does not persist between agent tool calls, so run everything in one PowerShell call:

```powershell
$env:IDF_TOOLS_PATH = "$HOME\.espressif"; $env:PATH = "C:\Espressif\tools\idf-python\3.11.2;" + $env:PATH; . $HOME\esp\v5.5.2\esp-idf\export.ps1 *> $null; idf.py -C firmware build
```

**Firmware:**

```bash
idf.py -C firmware build                 # target esp32s3, from sdkconfig.defaults
idf.py -C firmware build flash monitor
idf.py -C firmware menuconfig            # every component tunable is under its Kconfig menu
idf.py -C firmware fullclean             # when target or config changes leave the build inconsistent
idf.py -C firmware/test/uart_comm build  # the HIL test app
```

The no-log variant (`CONFIG_DIAG_CONSOLE_LOG=n`) is the one build run from inside `firmware/`, because `-C` with `-B` splits the variant's paths:

```bash
idf.py -B build_variant -D SDKCONFIG=build_variant/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.nolog" build
```

**Native unit tests** (no IDF export needed; they need Python 3 with PyYAML):

```bash
cmake -S firmware/test/native -B firmware/test/native/build -G Ninja
cmake --build firmware/test/native/build
ctest --test-dir firmware/test/native/build --output-on-failure
firmware/test/native/build/native_tests -tc="*SetTiming*"    # one test (doctest filter)
```

**Definitions** (after editing anything in `protocol/defs/`, regenerate and commit the outputs; the generator needs PyYAML, which the IDF Python env and this machine's Python both have):

```bash
python tools/gen/gen.py            # regenerate
python tools/gen/gen.py --check    # what CI runs
python -m unittest discover -s tools/gen/tests
```

**Hardware-in-the-loop and smoke tests** (from `firmware/test/uart_comm/`, after `pip install -r requirements.txt`; the board's ports are in `plans/overhaul-status.md`):

```bash
pytest pytest_uart_comm.py --port COM3                           # needs the HIL test app flashed
pytest pytest_uart_comm.py --port COM3 -k test_echo_max_payload   # one test
python smoke_main_firmware.py --port COM3                         # needs the main firmware flashed
```

**Build traps:**
- **Only what `main` requires is built.** `firmware/CMakeLists.txt` sets `COMPONENTS main esp_vfs_console`, so an IDF component is compiled only when a `REQUIRES` or `PRIV_REQUIRES` chain from `main` reaches it. A missing requirement fails the build with a header that is not found. A component that is only linked in drops out without an error: `esp_vfs_console` is named because without it the console leaves the USB-Serial-JTAG driver for the ROM's output routine. After changing a requirement, compare `idf.py -C firmware size-components` with the output from before.
- **A changed Kconfig default doesn't reach an existing `sdkconfig`.** After a pull that changes one, delete `firmware/sdkconfig` and `firmware/test/uart_comm/sdkconfig` before building, or the old values stay.
- **Restoring an older sdkconfig.** A copied backup may retain its old timestamp, so Ninja can leave the bench settings in generated headers. Run `idf.py -C firmware reconfigure build flash` when restoring normal firmware; check `firmware/build/config/sdkconfig.h` and verify bench commands are refused on the board.
- **Zero warnings.** The builds have none today, and CI fails on any.
- **One build at a time.** In F4a three IDF builds ran together, and a timing-sensitive native test failed under the load. `tools/validate.py` runs everything in sequence; do the same by hand.
- **A compiler crash in an IDF file.** Cold builds on this machine used to stop with `internal compiler error: Segmentation fault` (`during RTL pass: ira`) in IDF's `esp_lcd_panel_rgb.c`. The firmware no longer compiles that file. If another IDF file crashes the compiler, run the build again and Ninja resumes. `tools/validate.py` retries once and says so in the result.
- **A new pure source file** must also be added to `firmware/test/native/CMakeLists.txt` (see [firmware/test/CLAUDE.md](test/CLAUDE.md)).

## Architecture

**Startup** (`firmware/main/main.cpp`): `diag::init()`, `UartComm`, the legacy managers, the core, then `diag::Reporter`. A manager or the core that fails to start is reported and left out, and the rest keep running, so destructors must be safe at any point.

**Layering** (each is an ESP-IDF component under `firmware/components/`):
- `motor_hal` (`motor_hal::Motor`) and `rotary_encoder_hal` (`rotary_encoder_hal::Encoder`) are move-only hardware wrappers. Scarce peripherals come from static pools (`McpwmPool`, `PcntPool`) guarded by a FreeRTOS mutex.
- `pid` is pure math. `motor_controller` combines one motor, one encoder and the PIDs.
- `motor_manager`, `imu_bmo_055` and `time_sync` are the legacy protocol-facing managers. Each registers UART callbacks, owns its tasks, and keeps its payload codec in a pure module.
- `protocol/` (at the repo root, an IDF component through `EXTRA_COMPONENT_DIRS`) is the pure wire library: CRC, framing, the frame `Scanner`, `wire_codec`, and the generated registry, diag code table and descriptor format.
- `uart_comm` is the transport: the driver, the RX and TX tasks, callbacks, the command pool and the stream slots.
- `rtos_util` provides `StoppableTask`, `TaskTimer` and `msToTicksCeil`.
- `core_os` holds thin OS adapters (clock, `Mutex`, `Event`, `PeriodicTask`, `OneShotTimer`, the diag sink, the `KvStore` on NVS, the boot info) with an ESP-IDF backend and a native one. The backend is picked by include dir, never by `#ifdef`.
- `core` is the new core (F3 onwards): the module registry, configurable built-in loop instances, the param service, the layout, NVS A/B persistence, safe mode, the boot, and the command, action and group services of `docs/protocol/core.md`. It is pure: only `core_os`, `protocol/` and std, so it also builds natively. Its `CoreLink` adapter owns the UART side. Until the legacy managers are ported, it runs beside them with the configured mock instances (`mod_mock`).
- F4c adds source bindings (`Sampled<T>`) and boot-allocated cross-loop triple
  buffers. Source storage belongs to the consumer loop and survives arm
  transitions. `CORE_MAX_LINK_VALUES` bounds each ordered loop pair; see the
  core README for ownership, sampling and age rules.
- `diag` reports errors to the host as codes (wire v3, `docs/protocol/diag.md`). Record macros queue records; real-time counts and latched conditions update fixed atomic state. `diag::Reporter` sends events and answers queries. Only the managers, the core (through the `core_os` sink) and `main` report. The hardware layers return errors and don't depend on `diag`.

## Concurrency rules

- **Task lifecycle:** create every task with `rtos_util::StoppableTask`. Never use bare `xTaskCreate` / `vTaskDelete`.
  - Task bodies loop on `stopRequested()` and sleep with `sleepFor` / `sleepUntil`.
  - Every other blocking wait needs a bounded timeout.
  - Owners call `stop()` in their destructor, which waits for the body to return, before freeing anything the task uses.
- **Tick rate:** `CONFIG_FREERTOS_HZ` is 100 (10 ms ticks). Nothing real-time depends on it; fixed-rate loops use `esp_timer` (see "Periodic timers").
  - `pdMS_TO_TICKS` rounds down, so any value below 10 ms becomes 0 ticks. Convert small or configurable ms values with `rtos_util::msToTicksCeil` (`tick_util.hpp`).
  - `vTaskDelay(n)` can return up to one tick early. Where a delay is a minimum (sensor settling), use `msToTicksCeil(ms) + 1`.
- **Periodic timers:** fixed-rate loops use `rtos_util::TaskTimer`: a periodic `esp_timer` whose callback only notifies a `StoppableTask`. The task takes the count with `ulTaskNotifyTake` (bounded timeout) and counts extra periods as overruns. Owners serialize timer calls under their mutex and call `destroy()` (fenced) before stopping the task.
- **UART callbacks:** `registerCallback(type, cb, registration)` fills in a `uart_comm::CallbackRegistration`, which unregisters on destruction. Unregistering waits for a callback that is already running.
  - Destructors reset their registrations **first**.
  - Registrations must not outlive the `UartComm` (its destructor aborts if one is still active).
  - Callbacks run on the RX task, so they must not block. Hand work off through a queue or a mailbox.
- **Sending:**
  - `sendCommand()` takes a buffer from a preallocated pool. `ESP_ERR_NO_MEM` means the pool is empty.
  - `sendData()` writes a latest-wins stream slot. It never blocks or allocates, so call it freely from a control tick. Open the channel once at setup with `openStream(channel, max_len)`.
- **`RxFrame` is about 1 KB** (`MAX_PAYLOAD` is 1024). Don't put an `RxFrame` or a `MAX_PAYLOAD` buffer on a task stack without checking that task's stack mark, and remember that a queue of them costs 1 KB per entry.
- **Stack marks** are checked on hardware. Every task keeps more than 1 KB free. The stack log walks configured loop tasks by id; add other new tasks to the name list in `firmware/main/main.cpp`, which logs the marks.
- **Core loops:** a loop never waits for the config task. Param batches reach it only through its one staging slot, and the config task reads values from its own shadow copy. See [core/README.md](components/core/README.md), "Loops and concurrency".

## Conventions

### Real-time paths

These paths are real-time:
- the motor control tick (`MotorManager::updateTaskLoop` work, `MotorController::update`, `PidController::compute`)
- the core loop tick (`core::Loop::runTick`, every `Instance::tick` in a loop whose realtime flag is true), the staging swap inside it, and the params' `OnChange` hooks
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

Every protocol on top of the `uart_comm` frame follows these rules:
- **Version byte first.** Every payload starts with `kProtocolVersion`. Any change to the wire format bumps it. No backward compatibility is kept: the host and the firmware are updated together.
- **Request correlation.** Commands carry a host-chosen `seq` byte right after the version, and the response echoes it. A response to a header that could not be parsed uses `0xFF` (`kUnparsedHeaderByte`) for the subcommand and for the id fields. ESP-initiated requests carry their own id.
- **Encoding.** All multi-byte fields are little-endian. Floats are IEEE-754 binary32. Encode and decode field by field with the bounds-checked `drivelink::protocol::wire::Reader` / `Writer` (`protocol/include/drivelink/protocol/wire_codec.hpp`). Never `memcpy` a whole struct to or from the wire, and never write a component-local byte-order helper.
- **Exact lengths.** A fixed-size body must be exactly its size, or it is rejected. A variable-size body carries an explicit count, and `count × entry size` must equal the remaining length.
- **Result and detail.** A response carries `result` and then a `detail` byte that names the check that failed (`None` on success). Each failure site sets its own detail. Detail values are part of the wire format: add new ones, never renumber. Every command gets a response, including one dropped because a queue is full.
- **Untrusted input.** Reject invalid host input with an error result code and apply nothing. Invalid means a wrong length, an unknown subcommand or id, a non-finite float, a GPIO outside the allowed set, or an invalid enum value. A valid value outside its range is clamped, and the response reports the value actually applied.
- **IDs from the registry.** MSG_TYPE values and stream channels come only from `protocol/defs/registry.yaml`, through the generated `gen/registry.hpp`. Never hard-code one, and never add a Kconfig option for one.
- **`docs/protocol/` is the spec.** It defines the wire format, and is updated in the same commit as any wire change. The legacy managers are the exception during the overhaul: their wire format is their code and native tests (see [legacy-managers.md](components/legacy-managers.md)).

### Errors, failures and memory

- Check every ESP-IDF return value, and log failures with `esp_err_to_name`.
- **Report errors to the host through diag** (the codes are listed in `docs/protocol/diag-codes.md`):
  - Managers and `main` use `DIAG_ERROR` / `DIAG_WARN` for errors and warnings the host should know about. These write the console line and queue a code.
  - Frequent events are counted with `DIAG_COUNT`; recoverable active conditions use `DIAG_RAISE` on `kind: latched` codes. These atomic paths and latched `DIAG_CLEAR` are allowed on real-time paths.
  - Each new code goes in `protocol/defs/diag_codes.yaml`, then regenerate. Call sites write `diag::codes::<source>::k<Name>`, through the hand-written shim `firmware/components/diag/include/diag_codes.hpp`.
  - Plain `ESP_LOGx` is for info, for programmer errors, and for host commands rejected with a result code (the response already tells the host).
  - A manager constructor that fails reports `<source>.InitFailed` with the failing stage in `arg0`.
  - Recoverable conditions (a stalled timer, failing syncs) are reported once when they start and `DIAG_CLEAR`ed on recovery or when the task watching them stops, not on every occurrence. A real-time task only counts; a non-real-time task watches the count and reports.
  - `CONFIG_DIAG_CONSOLE_LOG=n` compiles out all first-party console text (`firmware/CMakeLists.txt` sets `LOG_LOCAL_LEVEL=ESP_LOG_NONE` on `firmware/components/` and `firmware/main/`). Code must build warning-free that way too, which CI checks.
- **Never abort on runtime errors** that the host or the hardware can cause. Return `esp_err_t` or `bool`, expose `isValid()`, and log the failure.
  - No `ESP_ERROR_CHECK` anywhere.
  - `assert` / `configASSERT` / `abort` are only for invariants that indicate a programmer error. One example: `UartComm` being destroyed while registrations are still active.
- **Exceptions and RTTI are disabled** (sdkconfig).
- **Allocate only at setup or configure time:** constructors, `start()`, and create/configure commands. Never allocate on steady-state paths. Size containers up front (`reserve` / `resize`).
  - Running out of memory is treated as fatal. Plain `new` / `std::make_unique`, which abort on failure, are acceptable.
- **NVS** belongs to the core (`core_os::KvStore`; see [core/README.md](components/core/README.md)). An NVS write stalls the caches of both cores for milliseconds, so it never runs on a real-time path, and SAVE and FACTORY_RESET are refused while armed.

### Definition of done

A change is done when all of the following are true:
- **Builds:** `python tools/validate.py full` passes. That means: `idf.py -C firmware build` (also with `firmware/sdkconfig.ci.nolog`) and `idf.py -C firmware/test/uart_comm build` succeed with zero warnings, the generated files are current (`python tools/gen/gen.py --check`), and the native tests, the generator's unit tests and the host tests pass. CI runs the same checks through the same script.
- **Docs in the same commit:**
  - Any protocol change updates `docs/protocol/` in that commit. A public API or Kconfig change updates the component's README, if it has one.
  - A change to an architecture, concurrency or convention rule updates the file that holds it: this file, an area file from the table above, `firmware/components/CLAUDE.md`, or the root `CLAUDE.md` for a cross-cutting rule.
- **Overhaul status:** during the overhaul, every PR updates `plans/overhaul-status.md` (see the root `CLAUDE.md`).
- **Native tests for pure logic:** new or changed pure-logic code (the core, PID, filters, estimators, protocol encode/decode) comes with native unit tests (`firmware/test/native/`, or `protocol/test/` for the `protocol/` library), and they pass.

### Files not to edit

- **Generated files:** don't edit anything under `firmware/build/` (or another build dir such as `build_variant/`) or the generated `sdkconfig` by hand. Everything under `protocol/include/drivelink/protocol/gen/` and the generated tables in `docs/protocol/` (`registry.md`, `diag-codes.md`, `types/`) come from `protocol/defs/` through `tools/gen/gen.py`. Edit the YAML and regenerate.
- **Vendor code:** `firmware/components/imu_bmo_055/vendor/bno055.c` / `bno055.h` are the Bosch driver.
- **Naming:** the component and namespace are spelled `imu_bmo_055`, but the part is the BNO055.

F4a groups are preallocated and use single-attempt atomic slot ownership;
loops never wait for RX or configuration. Group snapshots read plain signals
only on their owning loop. Command inputs stay gated disarmed until F4b.
