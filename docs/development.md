# Development commands and validation

Run commands from the repository root unless a section says otherwise.
This is the operational reference; agent constraints stay in the scoped
`CLAUDE.md` files. Host build and usage details are in [host/README.md](../host/README.md).

## Commands

**All the checks, one command** (from the repository root, in any shell; it exports ESP-IDF itself):

```bash
python tools/validate.py quick             # generator, native tests, this OS's host tests; no IDF
python tools/validate.py full              # every check; run it before a PR
python tools/validate.py --only fw-nolog   # one check (--list names them)
```

It runs the checks one at a time, skips a check whose code is unchanged since it passed, and writes the logs and a summary to `build/validate/`. See [Validation](#validation-toolsvalidatepy) below. The commands below are the ones it runs, for working on one build by hand.

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

**Hardware-in-the-loop and smoke tests** (from `firmware/test/uart_comm/`, after `pip install -r requirements.txt`; the board and its ports are in [bench.md](bench.md)):

```bash
pytest pytest_uart_comm.py --port COM3                           # needs the HIL test app flashed
pytest pytest_uart_comm.py --port COM3 -k test_echo_max_payload   # one test
python smoke_protocol_firmware.py --port COM3                      # needs the main firmware flashed
```

**Build traps:**
- **Only what `main` requires is built.** `firmware/CMakeLists.txt` sets `COMPONENTS main esp_vfs_console`, so an IDF component is compiled only when a `REQUIRES` or `PRIV_REQUIRES` chain from `main` reaches it. A missing requirement fails the build with a header that is not found. A component that is only linked in drops out without an error: `esp_vfs_console` is named because without it the console leaves the USB-Serial-JTAG driver for the ROM's output routine. After changing a requirement, compare `idf.py -C firmware size-components` with the output from before.
- **A changed Kconfig default doesn't reach an existing `sdkconfig`.** After a pull that changes one, delete `firmware/sdkconfig` and `firmware/test/uart_comm/sdkconfig` before building, or the old values stay.
- **Restoring an older sdkconfig.** A copied backup may retain its old timestamp, so Ninja can leave the bench settings in generated headers. Run `idf.py -C firmware reconfigure build flash` when restoring normal firmware; check `firmware/build/config/sdkconfig.h` and verify bench commands are refused on the board.
- **Zero warnings.** The builds have none today, and CI fails on any.
- **One build at a time.** In F4a three IDF builds ran together, and a timing-sensitive native test failed under the load. `tools/validate.py` runs everything in sequence; do the same by hand.
- **A compiler crash in an IDF file.** Cold builds on this machine used to stop with `internal compiler error: Segmentation fault` (`during RTL pass: ira`) in IDF's `esp_lcd_panel_rgb.c`. The firmware no longer compiles that file. If another IDF file crashes the compiler, run the build again and Ninja resumes. `tools/validate.py` retries once and says so in the result.
- **A new pure source file** must also be added to `firmware/test/native/CMakeLists.txt` (see [test rules](../firmware/test/CLAUDE.md)).

## Validation (`tools/validate.py`)

One script holds the list of checks and their commands. CI calls it with `--only`, so a check runs the same way locally and in CI. It needs only Python 3 (with PyYAML for `gen` and `native`), and runs from the repository root in any shell.

| Check | What it runs | Toolchain it sets up |
|---|---|---|
| `gen` | `gen.py --check`, the generator's tests, the script's own tests (`tools/tests`), `protocol.py` | The Python that runs the script |
| `native` | Configure, build and ctest of `firmware/test/native` | `g++`, `cmake` and `ninja` from `PATH` (Strawberry on this machine) |
| `host-win` | The `windows-mingw` preset: build, ctest, `install_smoke.py`. Windows only | Qt's MinGW, CMake and Ninja, first on `PATH` |
| `host-linux` | The `linux` preset: build, ctest, `install_smoke.py` | On Windows it runs inside WSL (the default distro, or `DRIVELINK_WSL_DISTRO`) |
| `fw`, `fw-nolog`, `fw-hil-app` | `idf.py build` of the firmware, its no-log variant and the HIL test app, each from inside its app, failing on any `warning:` | ESP-IDF: the caller's export if there is one. Otherwise, on Windows, `export.ps1` with the two fixes of this guide |

- **Modes.** `quick` is `gen`, `native` and the host check of this OS. `full` is every check. `--only` and `--skip` pick by name. The summary names the checks a run left out; only a `full` run with nothing left out is the gate for a PR.
- **One at a time.** Checks never run in parallel: each build already uses every CPU, and the tests have timing bounds. `--jobs` caps the CMake builds. `idf.py` has no job option for Ninja.
- **Unchanged checks are skipped.** A check that passed shows as `pass (unchanged)` until a file it covers changes. `--force` runs it anyway.
  - A change under `plans/`, `.claude/`, `.agents/` or `.github/` reruns nothing. A change under `docs/` or to a `.md` file reruns only `gen`. Any other change reruns every check.
  - The state is `build/validate/state.json`. Toolchain changes are not seen: use `--force` after one.
- **Build directories** are the ones the commands in this guide and `host/README.md` use, so they stay warm between runs. The WSL build is in `~/.cache/drivelink/<worktree>/host-linux` inside the distro. That is on the Linux filesystem (no rename locks) and not in `/tmp`, which WSL keeps in RAM and loses when the idle VM stops. Delete it there when the worktree is gone.
- **ccache.** On Windows the IDF export turns on ccache, so the first IDF build of a new worktree mostly reuses another worktree's objects. A cached object replays its warnings, so the warning scan still sees them.
- **Warnings.** The scan sees only the files compiled in that run. After a run that found warnings, the next run deletes that app's build directory and compiles everything. So fix the warning; a rerun never makes it go away. CI always builds from clean.
- **Compiler crashes.** A step that fails with `internal compiler error` in its output runs once more (see "Build traps" in this guide). If it then passes, the result says `pass (after one retry of a compiler crash)`. Nothing else is retried: a failing test fails the check.
- **Missing tools.** Every check's toolchain is looked up before anything runs. A check that can't run shows as `UNAVAILABLE` and fails the run.
- **Results.** One line per check with its time. A failure prints the end of its output. The full output is `build/validate/<check>.log`, and `build/validate/summary.txt` is the text for the PR's validation section: it has the revision and says what was not run.
- **Agent report.** Every completed validation invocation also writes
  `build/validate/summary.json` automatically; no extra flag is needed. Read this
  first to establish verification state, then open only the relevant log.
  `--list`, argument errors and interrupted/unhandled failures do not publish a
  report; the file describes the last completed invocation. Check its timestamps
  and source before using it as evidence for current work.
- **Adding a check:** one `Check` entry in `CHECKS`, and its name in the CI job that has its toolchain.
- **Hardware.** The script never flashes a board or opens a serial port. The HIL and smoke tests above stay separate and explicit.

### JSON report (schema version 1)

- Top-level fields: `schema_version`, UTC `started_at`/`finished_at`, `source`
  (`revision`: full Git SHA, `dirty`: boolean; both null if unknown), `request`
  (`mode`, `only`, `skip`, `force`, `fail_fast`), `status` (`passed`/`failed`),
  `full_gate_passed`, `hardware_status` (always `not_run`), `omitted_checks`,
  and `checks` in validator order, for checks supported on the current OS.
- Each check has `name`, `status` (`passed`, `reused`, `failed`, `unavailable`,
  `not_selected`, `skipped_after_failure`), `input_fingerprint`,
  `duration_seconds`, absolute `log_path`, `failure_reason`, `diagnostic_excerpt`,
  `execution` and `compiler_crash_retried`. Inapplicable/unknown values are null;
  fingerprints are null in CI. Unavailable/unexecuted checks never point to stale logs.
- `execution` records the actual attempt's `source` and UTC start/end timestamps.
  A reused check retains its original execution; its duration is null because no
  test ran now. Old cache entries reuse as before but have null execution/retry
  provenance. Missing log files produce null paths. The top-level source always
  identifies the current invocation, not the cached execution.
- `full_gate_passed` requires `full` mode, no omitted supported checks and no
  failures/unavailable/skipped checks. A partial run can have `status: passed`
  while this field is false. Hardware and toolchain freshness are not certified;
  existing `--force` rules still apply. Reports are replaced atomically.

## CI (`.github/workflows/ci.yml`)

Runs on every pull request and on pushes to `main` and `host_overhaul`. Each job provides a toolchain and calls `tools/validate.py --only <checks>`; the commands are in the script.
- `fw`, `fw-nolog` and `fw-hil-app`, one job each, in the `espressif/idf:v5.5.2` container.
- `gen` and `native` on Ubuntu.
- `host-linux` in an `ubuntu:26.04` container. It also covers the installed CMake packages and the Linux PTY serial path.
- There is no Windows job yet (H1a adds one). `host-win` runs only locally.

## Guarded bench builds

Use `python tools/bench_fw.py <mode>` rather than editing the normal sdkconfig.
The helper exports IDF; flashing modes default to COM3 (`--port` overrides it).
Read the ports and traps in [bench.md](bench.md) and the current board state in
[host-status.md](../plans/host-status.md#board-state) before a run.
`--dry-run` after a subcommand prints its commands without exporting IDF,
building or flashing. Logs are in `build/bench_fw/<mode>.log`.

| Mode | Purpose |
|---|---|
| `bench` | Builds and flashes `sdkconfig.bench` in isolated `firmware/build_bench/`; enables BenchCrash and BenchStall. |
| `normal` | Reconfigures/builds normal firmware and refuses to flash if bench or any XiP/code/rodata relocation option is enabled. |
| `xip` | Builds/flashes `sdkconfig.xip` in `firmware/build_xip/`; verifies bench off and XiP, instruction fetch and rodata relocation on. |
| `ota-image` | Builds a distinct candidate in `firmware/build_ota/`, verifies normal bench/XiP settings, and never flashes. Upload `firmware/build_ota/drivelink_v2.bin` through the CLI OTA workflow. |

All modes verify rollback, the custom dual-slot partition layout and 16 MB
flash. NVS stays at 0x9000/0x6000. Never erase flash as an update step.
Always restore `normal` after bench/XiP checks and verify the board refuses
bench commands. XiP measurements are in
[the F7b record](../plans/archive/f7b-implementation.md).
OTA commands and recovery are in [host/README.md](../host/README.md#ota-f9)
and [the OTA spec](protocol/ota.md).

## Loop-affinity checks

Bench builds record initial and first pinned post-callback affinity/CPU in
fixed internal storage. Main prints `Bench affinity` snapshots (-1 means no
affinity); loops never log them. Normal builds omit this observation.

`smoke_safety_firmware.py --any-core` checks automatic initial placement and
restores original placement. `host/test/loop_affinity_smoke.py --cli <cli>
--endpoint serial://COM3 --console COM4` checks discovered choices, Any-core
default, pending edits, persistence, ticking mocks and stacks above 1 KiB, then
restores a clean loops-only layout. Add `--bench` to require initial no-affinity
and post-tick FPU pinning; restore normal afterward. Close console before cleanup
reboots. Without console options the workflow runs on vdev in host integration.
Native loop tests cover requested affinity, saved 0/1 values, missing params,
invalid choices, DISCARD/REVERT and safe-mode edits.
