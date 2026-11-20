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

**Native unit tests** (no IDF export needed; CMake 3.20+, a C++23 compiler,
Ninja and Python 3 with PyYAML). On Windows, run:

```powershell
python tools/validate.py --only native --force
```

The validator explicitly uses `C:\msys64\ucrt64\bin\gcc.exe` and `g++.exe`
(MSYS2 UCRT64 GCC 16.2), Qt's CMake/CTest and Ninja, and builds in
`firmware/test/native/build/ucrt64/`. It puts the UCRT64 runtime DLLs first
on the check's PATH. It never falls back to Strawberry or Qt's MinGW compiler;
keep their build directories separate. To run one test by hand:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
firmware/test/native/build/ucrt64/native_tests.exe -tc="*SetTiming*"
```

On Linux (including the `ubuntu:26.04` native-tests CI container):

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
| `gen` | `gen.py --check`, `version.py --check` (every consumer names the root `VERSION`), the generator's tests, the script's own tests (`tools/tests`), `protocol.py`, the GUI `Theme` (`gen_theme.py --check` and its tests), the third-party notices (`gen_notices.py --check` for the GUI, the firmware and the CLI and agent packages' `host/notices/`, and its tests) | The Python that runs the script |
| `native` | Configure, build and ctest of `firmware/test/native` (C++23) | Windows: explicitly named MSYS2 UCRT64 GCC 16.2 and Qt CMake/CTest/Ninja, isolated `build/ucrt64/` and runtime PATH. Linux: GCC, CMake/CTest and Ninja from `PATH` |
| `host-win` | The `windows-mingw` preset: build, ctest (including the offscreen GUI tests), `install_smoke.py`. Windows only | Qt's MinGW GCC 13.1.0 (version checked), CMake and Ninja, first on `PATH`; Qt 6.12.0 from `C:/Qt/6.12.0/mingw_64` |
| `installer` | `tools/package_win.py`: the `windows-mingw-release` preset, the `windeployqt` staging directory, its notices coverage and a self-contained smoke (Qt and MinGW off `PATH`), then the Inno Setup installer in `build/installer/`. Windows only | As `host-win`, plus Inno Setup 6 (`ISCC`): `winget install --id JRSoftware.InnoSetup -e` |
| `host-linux` | The `linux` preset (with the GUI against Ubuntu's Qt 6.10): build, ctest (not label `agent`), `install_smoke.py` | On Windows it runs inside WSL (the default distro, or `DRIVELINK_WSL_DISTRO`). It first runs `tools/setup_linux.sh --check host` and names the fix when a package is missing |
| `agent` | The `linux-no-gui` preset (GUI off): build `drivelink-agent`, then ctest with label `agent` (`agent-unit`, `agent-integration`: the agent against the vdev and the CLI). `host-linux` skips that label (`-LE agent`), so the tests run once | As `host-linux` (WSL on Windows, build in `~/.cache/drivelink/<worktree>/agent`), but it runs `tools/setup_linux.sh --check agent`: no Qt needed. Part of `quick` on Linux only |
| `deb` | `tools/package_linux.py`, the H3c packages for the Ubuntu release it runs on. It builds the `linux-release` preset (`linux-release-no-gui` where Ubuntu's Qt is below 6.10, so 24.04). Each package is staged under `/usr`: stripped programs, licence and notices in `/usr/share/<package>`, which minimised systems keep, unlike `/usr/share/doc`. The stages bundle no shared library. The staged programs are smoked, the agent with the shipped `agent.json` and then with a vdev device. The packages are `build/deb/<package>_<version>~ubuntu<release>_<arch>.deb`: `drivelink-cli`, `drivelink-agent` (with `host/agent/deploy/`) and, on 26.04, the GUI's `drivelink`. Depends is `dpkg-shlibdeps`' list plus what it cannot see: the QML modules and `drivelink-cli (= version)` for the GUI, `passwd` for the agent | As `host-linux` (WSL on Windows; the stage and build are in `~/.cache/drivelink/<worktree>/linux-release` there). Off Windows it checks the `tools/setup_linux.sh` profile the release needs (`host`, or `agent` without the GUI). Installing them is CI's `linux-install` job (`installed_packages_smoke.py`, `installed_gui_smoke.py --deb`) or `deploy_check.py` on a host with systemd |
| `fw`, `fw-nolog`, `fw-hil-app` | `idf.py build` of the firmware, its no-log variant and the HIL test app, each from inside its app, failing on any `warning:` | ESP-IDF: the caller's export if there is one. Otherwise, on Windows, `export.ps1` with the two fixes of this guide |
| `fw-package` | `idf.py reconfigure build` of the firmware (the version is read at configure time), then `tools/package_fw.py`: checks the app image's embedded version and project name, and writes `build/firmware/drivelink-firmware-<version>/` (a merged image for 0x0, the parts at their offsets, the OTA app, the ELF, `flash_args.json`, `README.txt`, licence, `firmware/notices/THIRD_PARTY_NOTICES.txt`, `SHA256SUMS`) | As `fw` (esptool comes with ESP-IDF) |

- **Modes.** `quick` is `gen`, `native` and the host check of this OS (on Linux also `agent`). `full` is every check. `--only` and `--skip` pick by name. The summary names the checks a run left out; only a `full` run with nothing left out is the gate for a PR.
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
- **Process output.** Commands write to a temporary file that the validator tails.
  Completion follows the command's exit, so a crashed test's descendant retaining
  stdout cannot leave validation waiting forever for pipe EOF. Partial final lines
  and nonzero exit codes are retained; failed tests are not retried.
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

## Version (`VERSION`)

The root `VERSION` (semver) is the one version of the repository.
`cmake/DriveLinkVersion.cmake` gives it to every CMake project before
`project()`: the firmware and HIL app as `PROJECT_VER` (GET_INFO's
`firmware_version`; `bench_fw.py`'s `-D PROJECT_VER` wins), `protocol/`,
`libdrivelink/` and the superbuild, the GUI's About, the installer and the
`.deb` packages (with `~ubuntu<release>` appended). A build at a clean checkout of tag `v<VERSION>` uses it as is; any
other build appends `+g<sha7>`, and `.dirty` with modified tracked files (git is
read at configure time). An environment `DRIVELINK_VERSION_FULL`
(`<VERSION>[+suffix]`) wins in both: `validate.py` passes the Windows
checkout's version to the `deb` build in WSL, whose git cannot read a Windows
worktree. vdev reports `<VERSION>-vdev`. The `package.xml` files repeat it
literally. `python tools/version.py --check` (in `gen`) fails on any
mismatch, and `python tools/version.py` prints the full version.

To release: edit `VERSION` and the two `package.xml` files, merge, then push
tag `v<VERSION>` on that commit. CI runs every job on the tag; when all pass,
its `release` job checks the tag against `VERSION` and that every package
carries the plain version, and publishes a GitHub Release with
`drivelink-firmware-<version>.zip`, `DriveLink-<version>-setup.exe`, the ten
`.deb` packages (`<package>_<version>~ubuntu<release>_<arch>.deb`: the CLI and
agent for 26.04 and 24.04, the GUI for 26.04, each for amd64 and arm64) and
their `SHA256SUMS`.

## Linux packages (`tools/setup_linux.sh`)

The one list of Ubuntu packages, for the WSL distro and the CI containers
(26.04; the `agent` profile also on 24.04, where CI builds the packages
without the GUI). It installs only what is missing and never removes or
upgrades anything.

| Profile | Packages for | Used by |
|---|---|---|
| `base` | compilers, CMake, Ninja, Python 3, PyYAML, git | `gen`, `native` (CI native-tests) |
| `agent` | the `base` packages (the GUI-less superbuild; no Qt) | `agent` (WSL and CI), `deb` on 24.04 (CI) |
| `host` (default) | `base` plus Ubuntu's Qt 6.10 development packages, the QML modules (including Qt Quick Dialogs for the GUI's file picker) and the offscreen platform plugin | `host-linux` and `deb` on 26.04 (WSL and CI) |

```bash
wsl -d Ubuntu-26.04 -u root sh tools/setup_linux.sh host
```

`sh tools/setup_linux.sh --check <profile>` only reports what is missing; `host-linux`
runs it first. Later steps add profiles (ROS in H4a). `.gitattributes` keeps
`*.sh` at LF, because WSL runs the scripts from the Windows checkout.

## CI (`.github/workflows/ci.yml`)

Runs on every pull request and on pushes to `main` and `host_overhaul`. Each job provides a toolchain and calls `tools/validate.py --only <checks>`; the commands are in the script. The Ubuntu jobs install only git for the checkout; their packages come from `tools/setup_linux.sh` ("Linux packages" below), so `ci.yml` holds no package list.
- `fw`, `fw-nolog` and `fw-hil-app`, one job each, in the `espressif/idf:v5.5.2` container.
- `firmware-package` runs `fw-package` in the same container (git marked safe,
  so the version has its `+g<sha7>`) and uploads `drivelink-firmware`.
- `release` runs only on a pushed `v*` tag, after every other job passed; it
  is the only job with `contents: write` (see "Version" for what it checks and
  publishes).
- `gen` and `native` in an `ubuntu:26.04` container with `tools/setup_linux.sh base`.
- Host tests and host packages are separate jobs, so a test failure never hides
  the package's result and each job finishes sooner.
- `host-linux-tests` runs `host-linux` in an `ubuntu:26.04` container with `tools/setup_linux.sh host` (Ubuntu's Qt 6.10, the GUI's API floor). It also covers the installed CMake packages and the Linux PTY serial path. Both host checks run CLI golden/replay and device-model CTest cases; Linux additionally checks SIGINT during testgen START. The installed consumer uses typed model headers with JSON package discovery disabled.
- `agent` runs `agent` in an `ubuntu:26.04` container with `tools/setup_linux.sh agent` (no Qt), and uploads `agent-validation` on failure. `host-linux-tests` skips the `agent` ctest label, so each test runs once.
- `linux-package` runs `deb` in a matrix of `ubuntu:26.04` and `ubuntu:24.04`
  containers on `ubuntu-latest` (amd64) and `ubuntu-24.04-arm` (arm64), with
  `setup_linux.sh host` on 26.04 and `agent` on 24.04. Git is marked safe, so
  the version has its `+g<sha7>`. The 24.04 build also holds the GCC 13 floor
  on Linux. Each run uploads `drivelink-debs-<release>-<arch>`: the CLI and
  agent packages, plus the GUI on 26.04.
- `linux-install` takes each artifact into a clean container of its release
  and architecture with only git and Python. There it runs
  `installed_packages_smoke.py`:
  - install, then an upgrade from a relabelled lower version, during which
    the service is never restarted;
  - remove, reinstall and purge;
  - `systemd-analyze verify` and `udevadm verify`;
  - on 26.04, the upgrade from H2g's single `drivelink` package.

  On 26.04 it also runs `installed_gui_smoke.py --deb --vdev`: the installed
  GUI renders every screen against the installed vdev, and the screenshots
  are uploaded as `drivelink-installed-screens-<arch>`.
- `host-windows-tests` and `host-windows-package` run on `windows-2025` with
  Python 3.12. Both use `.github/actions/windows-qt`, which caches Qt 6.12.0
  `win64_mingw`, Qt's MinGW 13.1.0, CMake and Ninja under `C:/Qt` and verifies
  the SDK version. The tests job runs `host-win` (45-minute limit); the package
  job installs Inno Setup 6.7.1 with Chocolatey when the runner lacks it, runs
  `installer` (30-minute limit) and uploads the setup exe as the
  `drivelink-windows-installer` artifact. The validator and preset enforce the
  selected compiler; the runner's default compiler cannot replace it. Validation
  logs and JSON/text summaries are uploaded even on failure or timeout.
- The Windows installer uses aqtinstall pinned to upstream commit
  `5a49f2b391ae8ee1d3c0dd87f8b33c1d49a4deb7`: released 3.3.0 does not handle
  the split Windows Qt 6.11+ repositories. The workflow names the exact Qt
  version and `tools_mingw1310` variant; aqt verifies the downloaded archives.
  CMake and Ninja are Qt tool variants. Bump the cache key when changing the kit
  or installation recipe; no fallback cache may supply a different kit.

## Host concurrency and time checks

### GUI shell checks (H2a-2)

Both host checks include `gui-backend` (C++/Qt model and real-vdev lifecycle,
arm/DISARM/ack, uncertain outcomes and key/popup/scrim flows) and
`gui-screenshots` (Connect/Device at both sizes/themes), alongside the
existing `gui-quick` and `gui-gallery`. Linux additionally checks an in-place
Boot over the vdev PTY. They use offscreen/software rendering and the same
CTest targets in CI; no new validator check/toolchain is needed.
The backend's QtTest function watchdog is 60 seconds, below CTest's 180-second
suite limit, so Qt can report a blocked function before the outer timeout.
The safe-mode/link-loss test records its teardown/reconnect stages in failure logs.

Later steps add `gui-monitor`, `gui-tune`, `gui-panels` (H2f: custom
panels on vdev), `gui-settings` and `gui-remote` (H3b-1: the remote rules on
both OSes; on Linux also through `drivelink-agent` and vdev, so `host-linux`
runs the remote integration) to the same CTest run.

From `host/superbuild`, run `ctest --preset windows-mingw -R '^gui-'`
(or `linux`). The complete post-review vdev campaign and fourteen-scenario render
command are in [host/gui/README.md](../host/gui/README.md#shell-screenshots).
It can repeat the backend tests and writes 56 dimension-checked PNGs.
The bare-board GUI check is `gui_backend_tests benchBoard` with
`DRIVELINK_GUI_BENCH_ENDPOINT=serial://COM3` (see the GUI README). All
scenario provisioning and fault injection use fresh temporary vdev state.
The GUI app's direct `--endpoint ... --out DIR` mode uses the supplied device
without scenario injection. Native arming fixtures save slower loop rates
and watchdog settings so they survive a simulated reboot.

### Library checks

H1c adds `host-identity` to both host checks: CLI Unicode/byte limits,
SAVE/reboot/REVERT, immutable device id and armed rename with a mock actuator
through `async_probe`. Run the same workflow on the bare board with
`python host/test/identity_smoke.py --cli <cli> --endpoint serial://COM3 --probe <async_probe>`.
The script requires clean saved settings and idle groups and restores them.

The host checks include `host-concurrency` and a separate real-vdev `host-async`
consumer alongside unit, model, golden/replay, integration and install tests.
Concurrency coverage includes single and batched typed admission racing close;
transport cancellation runs outside operation-owner locks to avoid an I/O-join
lock cycle. This is stress coverage, not a forced scheduling interleaving.
`host/test/async_probe` takes one endpoint; on a bare loops-only serial device
it also checks Boot invalidation/reload, restoring the original idle groups.
See host/README.md for its preconditions and native scheduling distinction.

For a focused Linux GNU ThreadSanitizer run, use a separate build directory:

```sh
cmake -S host/superbuild -B ~/.cache/drivelink/tsan -G Ninja -DDRIVELINK_TSAN=ON
cmake --build ~/.cache/drivelink/tsan --target host_concurrency_tests host_tests --parallel
TSAN_OPTIONS=halt_on_error=1 ~/.cache/drivelink/tsan/test/host_concurrency_tests
TSAN_OPTIONS=halt_on_error=1 ~/.cache/drivelink/tsan/test/host_tests --test-case='*model*,*snapshot*'
```

This instruments the host library and selected concurrency/model cases; it is
not a firmware sanitizer campaign. Normal host checks retain their regular
toolchains. Run builds/tests one at a time, including this optional run.
`time_smoke.py` checks the production mapped-time fields as well as the existing
held-out residual/drift gates; run idle and `--traffic` for the CH343.

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

## Serial adapter in WSL (`tools/wsl_serial.py`)

`drivelink-agent` is Linux only, so hardware tests lend the board's CH343
(1a86:55d3) to WSL through usbipd-win. Once, in an admin shell, run
`usbipd bind --busid <id>` (`python tools/wsl_serial.py status` shows the id),
and once in WSL add your user to `dialout` (`sudo usermod -aG dialout $USER`;
without it the tty refuses to open: "Permission denied"). The tool never
binds and needs no admin. Never part of `validate.py`.

| Subcommand | Purpose |
|---|---|
| `status [--json]` | Matching adapters: busid, state (not connected, not shared, shared on Windows, attached to WSL) and the WSL tty. Exit 3 if none is plugged in. |
| `attach` | Refuses (exit 2) when the adapter is missing, ambiguous without `--busid`, or not bound; else attaches and prints the Linux tty. |
| `detach` | Detaches and waits until Windows has it back; a no-op when it is not attached. |
| `run [--] CMD...` | Attaches, runs `CMD` with `DRIVELINK_WSL_TTY` set (passed into `wsl.exe` through `WSLENV`), then always detaches, also on Ctrl+C or when `CMD` fails. Returns `CMD`'s exit code, or 4 if the detach failed. |

Options (before `CMD`): `--busid`, `--vid-pid`, `--distro`, `--timeout`,
`--usbipd`, and `--dry-run`, which prints the commands that would change
anything and runs none. Give the board back with `detach` before flashing or
using the host on Windows.

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
