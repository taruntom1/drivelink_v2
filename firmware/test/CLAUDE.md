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

## CI (`.github/workflows/ci.yml`)

Runs on every pull request and on pushes to `main` and `firmware_overhaul`.
- `idf.py build` in the `espressif/idf:v5.5.2` container, which fails on any compiler warning. It builds `firmware/`, then `firmware/` again with `firmware/sdkconfig.ci.nolog` (`CONFIG_DIAG_CONSOLE_LOG=n`), then `firmware/test/uart_comm`. Each app is built from inside its own directory.
- On Ubuntu: `tools/gen/gen.py --check`, the generator's unit tests, `protocol.py` and the native tests.
- The host Linux job builds `host/superbuild`, and also checks the installed CMake packages and the Linux PTY serial path.

F3d adds configurable-loop coverage (one through eight loops, sparse ids, empty
saved layouts, factory fallback, task failures, live rate rollback and the native
real-time guard). The guard fails unexpected violations automatically; deliberate
violation tests explicitly suppress its failure sink. Native and vdev link the
C allocator wrappers alongside the C++ allocation overrides.
