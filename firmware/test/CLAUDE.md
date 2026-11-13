# CLAUDE.md (firmware/test)

How the firmware tests are built and what they cover. The commands are in [firmware/CLAUDE.md](../CLAUDE.md) ("Commands"). Paths are relative to the repository root.

## Native unit tests (`firmware/test/native/`)

**What they cover:** everything that builds without ESP-IDF.
- The `protocol/` library: CRC, framing, scanner, wire codec and the generated definitions. Its tests live in `protocol/test/` and are compiled into this build.
- `core_os`'s native backend, with the in-memory KV store and its fault injection (power loss, failed and torn writes).
- The pure `core`: registry, bind API, values, loops, param batches, the pending layout and its validation, the wiring order, the NVS blobs and A/B persistence, safe mode, the boot, and a threaded end-to-end run.
- The legacy motor_manager, IMU, time_sync and diag protocol codecs, diag's slot table, the PID and the velocity estimator.

**How it is built:**
- A plain CMake project (target `native_tests`) with a vendored doctest (`firmware/test/native/third_party/doctest`).
- It builds `protocol/` as the host library `drivelink::protocol`. It compiles `core_os/backend/native`, the `core` sources (all but `src/idf/`) and the legacy sources directly with the host compiler.
- `firmware/test/native/shim/` stands in for the two IDF headers the legacy sources include: `sdkconfig.h` with the Kconfig defaults they read, and a no-op `esp_log.h`. The core needs neither.
- It needs no IDF export. On this machine, Strawberry Perl ships `g++` (MinGW-w64, posix threads), `cmake` and `ninja`.
- It needs Python 3 with PyYAML: the build runs the generator on the fixtures in `tools/gen/tests/fixtures` to make the type headers the tests include.

**Adding a source:**
- A new `core` source goes in `CORE_SOURCES` in `firmware/test/native/CMakeLists.txt`. It must not need the shims.
- A legacy source can join only if it builds without ESP-IDF beyond the shims. Keep codecs and math in their own files (`*_protocol.cpp`, `velocity_estimator.cpp`), apart from the code that touches FreeRTOS or drivers, and add them to `FIRMWARE_SOURCES`.
- If a legacy source reads a new `CONFIG_*` symbol, add it to `firmware/test/native/shim/sdkconfig.h`.

**Writing core tests:**
- The generator's fixture types serve as test module types (`tests/core_test_modules.hpp`): `fixture.full`, `fixture.sensor`, and the wiring set `actuator.fixture`, `feedback.fixture`, `fixture.wired`.
- A test builds a layout by writing it to a native `KvStore` (`writeLayout`) and booting a `Core` on it (`bootWith`). A reboot is a new `Core` on the same store.
- The native store stays in memory unless `native::openFileStore` attaches a state directory, which only the vdev does.
- Tests that use threads (the `core_os` timing and the threaded core run) keep loose timing bounds: a desktop scheduler can wake a thread 15 ms late.

## Hardware-in-the-loop tests (`firmware/test/uart_comm/`)

- A separate IDF app for `uart_comm`. Its `EXTRA_COMPONENT_DIRS` points at `../../components`, and it builds only the components its `main` requires (`set(COMPONENTS main)`).
- `pytest_uart_comm.py` runs against the flashed test app through a USB-UART adapter on GPIO43/44. `conftest.py` defaults `--baud` to 921600, the Kconfig default.
- `protocol.py` is the Python frame reference. `python protocol.py` is a hardware-free self-test with the golden frames, and CI runs it. Update it with any framing change.
- The test app logs the uart_comm RX and TX stack marks when idle.

## Smoke scripts for the main firmware

Standalone scripts in `firmware/test/uart_comm/`, for the **main** firmware, not the test app. Update them with any change to what they check.

- `smoke_main_firmware.py --port COMx` checks that diag, motor_manager, the IMU and time_sync answer with their current protocol versions, and that the IMU stream arrives on its registry channel. It creates no motor, so nothing can move.
- `smoke_core_firmware.py` exercises the core with mock instances and removes them again. `--safe-mode` needs a build with `CONFIG_CORE_BENCH_CRASH=y`; flash the normal firmware again afterwards.

The ports and the state of the bench board are in `plans/overhaul-status.md` ("Hardware notes").

## Host tests

The host library, CLI and vdev have their own unit and integration tests under `host/test`, built by `host/superbuild`. See `host/README.md`.

## Validation (`tools/validate.py`)

One script holds the list of checks and their commands. CI calls it with `--only`, so a check runs the same way locally and in CI. It needs only Python 3 (with PyYAML for `gen` and `native`), and runs from the repository root in any shell.

| Check | What it runs | Toolchain it sets up |
|---|---|---|
| `gen` | `gen.py --check`, the generator's tests, the script's own tests (`tools/tests`), `protocol.py` | The Python that runs the script |
| `native` | Configure, build and ctest of `firmware/test/native` | `g++`, `cmake` and `ninja` from `PATH` (Strawberry on this machine) |
| `host-win` | The `windows-mingw` preset: build, ctest, `install_smoke.py`. Windows only | Qt's MinGW, CMake and Ninja, first on `PATH` |
| `host-linux` | The `linux` preset: build, ctest, `install_smoke.py` | On Windows it runs inside WSL (the default distro, or `DRIVELINK_WSL_DISTRO`) |
| `fw`, `fw-nolog`, `fw-hil-app` | `idf.py build` of the firmware, its no-log variant and the HIL test app, each from inside its app, failing on any `warning:` | ESP-IDF: the caller's export if there is one. Otherwise, on Windows, `export.ps1` with the two fixes of `firmware/CLAUDE.md` |

- **Modes.** `quick` is `gen`, `native` and the host check of this OS. `full` is every check. `--only` and `--skip` pick by name. The summary names the checks a run left out; only a `full` run with nothing left out is the gate for a PR.
- **One at a time.** Checks never run in parallel: each build already uses every CPU, and the tests have timing bounds. `--jobs` caps the CMake builds. `idf.py` has no job option for Ninja.
- **Unchanged checks are skipped.** A check that passed shows as `pass (unchanged)` until a file it covers changes. `--force` runs it anyway.
  - A change under `plans/`, `.claude/`, `.agents/` or `.github/` reruns nothing. A change under `docs/` or to a `.md` file reruns only `gen`. Any other change reruns every check.
  - The state is `build/validate/state.json`. Toolchain changes are not seen: use `--force` after one.
- **Build directories** are the ones the commands in `firmware/CLAUDE.md` and `host/README.md` use, so they stay warm between runs. The WSL build is in `~/.cache/drivelink/<worktree>/host-linux` inside the distro. That is on the Linux filesystem (no rename locks) and not in `/tmp`, which WSL keeps in RAM and loses when the idle VM stops. Delete it there when the worktree is gone.
- **ccache.** On Windows the IDF export turns on ccache, so the first IDF build of a new worktree mostly reuses another worktree's objects. A cached object replays its warnings, so the warning scan still sees them.
- **Warnings.** The scan sees only the files compiled in that run. After a run that found warnings, the next run deletes that app's build directory and compiles everything. So fix the warning; a rerun never makes it go away. CI always builds from clean.
- **Compiler crashes.** A step that fails with `internal compiler error` in its output runs once more (see "Build traps" in `firmware/CLAUDE.md`). If it then passes, the result says `pass (after one retry of a compiler crash)`. Nothing else is retried: a failing test fails the check.
- **Missing tools.** Every check's toolchain is looked up before anything runs. A check that can't run shows as `UNAVAILABLE` and fails the run.
- **Results.** One line per check with its time. A failure prints the end of its output. The full output is `build/validate/<check>.log`, and `build/validate/summary.txt` is the text for the PR's validation section: it has the revision and says what was not run.
- **Adding a check:** one `Check` entry in `CHECKS`, and its name in the CI job that has its toolchain.
- **Hardware.** The script never flashes a board or opens a serial port. The HIL and smoke tests above stay separate and explicit.

## CI (`.github/workflows/ci.yml`)

Runs on every pull request and on pushes to `main` and `firmware_overhaul`. Each job provides a toolchain and calls `tools/validate.py --only <checks>`; the commands are in the script.
- `fw`, `fw-nolog` and `fw-hil-app`, one job each, in the `espressif/idf:v5.5.2` container.
- `gen` and `native` on Ubuntu.
- `host-linux` in an `ubuntu:26.04` container. It also covers the installed CMake packages and the Linux PTY serial path.
- There is no Windows job yet (H1 adds one). `host-win` runs only locally.

F3d adds configurable-loop coverage (one through eight loops, sparse ids, empty
saved layouts, factory fallback, task failures, live rate rollback and the native
real-time guard). The guard fails unexpected violations automatically; deliberate
violation tests explicitly suppress its failure sink. Native and vdev link the
C allocator wrappers alongside the C++ allocation overrides.

F4a adds native group tests for config/withdrawal, epoch wrapping, typed
command rejection and atomicity, sparse loop ids, 64-entry limits, subscription
dividers and concurrent mailbox coherence. Host integration exercises CLI
groups on vdev and verifies disarmed commands do not alter mock inputs.


F4b adds deterministic safety tests with native manual scheduling: ARM/disarm
races, command freshness across arm epochs, latched recovery/ACK, watchdog
feeds and grace, sparse non-real-time stalls, emergency force without a loop
transition, read-only limits/persistence, and sparse diagnostic exhaustion.
Host integration covers arming/refusals, command/keepalive runs and typed CSV.
The default-off bench stall command supports bare-board monitor/WDT checks.

`smoke_safety_firmware.py --port COM3` checks normal arming/watchdog behavior
with one temporary actuator mock. A bench-enabled build additionally runs
`--bench --wdt` to test stalls in real-time/background loops and the Task WDT
reset; inspect COM4 for the panic. It restores the saved layout and settings.

F4c adds native source binding/validation, ordering, capacity, persistence,
failed-producer, age/freshness and concurrent triple-buffer coherence tests.
Host integration discovers source descriptors and streams fusion values and
ages. `smoke_sources_firmware.py --port COM3` checks sources on the bare board
and compares the longest control tick with zero and sixteen linked values
in otherwise identical mock layouts. It requires an original layout with
loops only and restores it. Use COM4 concurrently to inspect stack marks.
