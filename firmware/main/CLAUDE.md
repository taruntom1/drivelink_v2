# CLAUDE.md (firmware/main)

Rules for `firmware/main/main.cpp`, which starts the firmware. The firmware-wide rules are in [firmware/CLAUDE.md](../CLAUDE.md). Paths are relative to the repository root.

## Startup order

- The order is `diag::init()`, `UartComm`, then `TimeSync` (which takes a reference to `UartComm`), then the core (`startCore()`), then `diag::Reporter`.
- Each manager is checked with `isValid()`. One that failed is reported (`system.ManagerStartFailed`), destroyed through its `std::optional`, and left out, so destructors must be safe at any point. The rest keep running.
- Before the core, main initialises NVS (`core_os::nvsInit()`). A full partition or one from a newer NVS version is erased and initialised again. Both that and a failure are reported (`core.NvsInitFailed`, arg0 1 erased, 2 failed). The core boots either way.
- The core:
  - It is built on the heap (`app_main`'s stack is small) from `coreConfigFromKconfig()` and main's module list (`kModuleTypes`, from `mod_mock::kTypes`, `mod_motor::kTypes`, `mod_motor::kHardwareTypes` and `mod_imu_bno055::kType`; each desc is checked against `defs::kTypeTable`).
  - main calls `init()`, then `boot(store)`, then `start()`. The `KvStore` is a static in main. What `boot` does is in `firmware/components/core/README.md`.
  - If one of them fails, main reports `core.InitFailed` (arg0 the stage: 1 registry, 2 loop task with id in arg1, 3 retired, 4 boot) and continues without the core.
  - It logs `Core started: ...` with the running, failed and configured instance counts, safe mode, whether the config came from NVS, and each running loop's id, real-time flag and period.
  - `CoreLink` (the core's UART adapter, which owns the `core_cfg` task) is created after the core. If it fails, main logs it and goes on.
- The Reporter comes last so its `Boot` event carries the final instance counts and the safe-mode flag. Records from the startup wait in the diag queue until then.
- `app_main` never returns. If `UartComm` fails, there is no link to report on: it logs to the console and idles.

## Rules

- **The `main` task stack is small** (5632 bytes) and `app_main`'s locals are large. Build anything big on the heap, and check the `main` stack mark after adding something that `app_main` starts. Instance `start()` runs on it during boot: F5a-2's MCPWM/PCNT driver setup took it to 848 bytes free at 4608, so it was raised by 1 KB.
- **Stack log.** main logs every task's stack high-water mark each `CONFIG_DRIVELINK_STACK_LOG_PERIOD_S` (30 s, 0 = off). Non-loop task names are a list in `main.cpp`: add new tasks to it. Loop tasks are logged by walking the core loop table and using each id.
  - It uses `xTaskGetHandle` and `uxTaskGetStackHighWaterMark()`. The second takes no kernel lock.
  - Never use `uxTaskGetSystemState()`: it scans every stack inside a critical section and stalls the control loop on the other core.
- **Bench-only options** in `Kconfig.projbuild`, default off in normal firmware:
  - `CONFIG_CORE_BENCH_CRASH` enables BenchCrash for the safe-mode check and BenchStall for monitor/Task WDT checks. Normal builds reject the op.
  - From the repository root, run `python tools/bench_fw.py bench` on the bare board, then `python tools/bench_fw.py normal` after the checks. Both export IDF and flash COM3 (`--port` overrides it); full output is in `build/bench_fw/<mode>.log`. `bench` uses the committed `firmware/sdkconfig.bench` fragment and isolated, git-ignored `firmware/build_bench/`, without touching `firmware/sdkconfig`. `normal` reconfigures and builds, then refuses to flash if `firmware/build/config/sdkconfig.h` enables `CONFIG_CORE_BENCH_CRASH`. Still verify bench commands are refused on the board after restoration.
  - `--dry-run` after either subcommand prints the commands without exporting IDF, building or flashing. This helper never runs as part of validation; its hardware-free tests run under `gen`.
