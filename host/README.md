# DriveLink host tools

`libdrivelink` provides serial/TCP sessions and the current core families.
`drivelink-cli` drives the ESP32 or `drivelink-vdev` with identical commands.
The library has no Qt or ROS dependency. All service results and events are
typed C++; JSON formatting belongs to the CLI, whose output stays unchanged.
The library includes asynchronous services, a live discovered device model,
pulled group buffers, managed command keepalives and timestamp mapping. CI builds
and tests it on Windows and Linux.
See [the library API](libdrivelink/README.md).

`drivelink-gui` provides the local Qt shell and backend: Connect (serial ports,
baud, a TCP address for vdev, recent devices by id, loading steps, protocol
mismatch) and Device (identity and Rename, loops with run time and budget, CPU,
limits, ARM checks, faults, SAVE/REVERT, reboot, factory reset, safe-mode exit),
the faults drawer, link-lost and reboot-notice overlays. Esc disarms through
focused text fields and popups; the status bar remains above page scrims. GUI
command groups send keepalives only, leaving input values unchanged. H2b-2
adds the firmware update dialog and the Device page's Firmware update card;
Layout, Tune and Monitor follow.
See [GUI usage, backend contracts and scenario screenshots](gui/README.md).

## Device identity (H1c)

Core v9 adds the immutable factory MAC and a user-set UTF-8 name. `info`
reports both; `name "base"` renames and `name ""` clears. Names allow up to
16 UTF-8 bytes without C0/C1 controls. Rename is live, also armed; SAVE persists
and REVERT restores it. Existing LP saved settings load with an empty name.

Both host checks run `host-identity` against vdev. Bare-board verification:
`python host/test/identity_smoke.py --cli <cli> --endpoint serial://COM3 --probe <async_probe>`.
It requires saved settings, idle groups and a disarmed normal device, uses a
temporary mock actuator, checks armed rename through the async consumer, and
restores name/layout/settings.

## Build

Requires CMake 3.24+, Ninja, a C++23 compiler (GCC 13.1 minimum) and Python 3. The first configure
downloads SHA256-pinned standalone Asio 1.30.2 and nlohmann JSON 3.11.3.
The `linux` and `windows-mingw` presets also build the GUI application and style
([gui/README.md](gui/README.md)), which needs Qt 6.10 or newer (Quick,
QuickTemplates2, Svg, QuickTest); `linux-no-gui` leaves it out. On Ubuntu
26.04 (WSL or CI) `sudo sh tools/setup_linux.sh host` installs every package.

```sh
cd host/superbuild
cmake --preset linux
cmake --build --preset linux --parallel
ctest --preset linux
python3 ../test/install_smoke.py --build build/linux
```

Windows PowerShell (Qt's installed MinGW; adjust the explicit preset compiler
path if Qt is elsewhere):

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;' + $env:PATH
cd host/superbuild
cmake --preset windows-mingw
cmake --build --preset windows-mingw --parallel
ctest --preset windows-mingw
python ../test/install_smoke.py --build build/windows-mingw
```

The binaries are under `build/<preset>/cli/`, `build/<preset>/vdev/` and
`build/<preset>/gui/`. The Windows preset finds Qt in `C:/Qt/6.12.0/mingw_64`;
the GUI tests prepend Qt's `bin` to their own PATH.
Keep the same MinGW runtime DLLs on PATH when running them.

From the repository root, `python tools/validate.py --only host-win host-linux`
runs both sequences above with the right toolchain for each, the Linux one
inside WSL (see [docs/development.md](../docs/development.md#validation-toolsvalidatepy)).
Windows host binaries use Qt's MinGW 13.1 exclusively; MSYS2 UCRT64 GCC 16.2
is reserved for firmware native tests. Never mix their runtime DLLs or build directories.
The Windows preset and validator enforce GCC 13.1.0; Linux GNU builds require
at least GCC 13.1. The Windows CI job provisions and caches Qt 6.12.0 plus that
MinGW kit; details are in [development.md](../docs/development.md#ci-githubworkflowsciyml).

Both host checks run the `host-model` CTest case against vdev, covering initial
load/reload, safe mode and configured/pending layouts. `test/model_probe` (with
`.exe` on Windows) is a read-only public model consumer used by that test and by
bench verification: pass it one endpoint such as `serial://COM3`. It loads and
reloads the complete snapshot and prints test TSV; it does not alter the CLI's
output formats or change the device.

Both host checks also run `host-concurrency` (dispatcher/order/cancel/close,
peer-EOF races, saturated DISARM, buffers, managed queues and time) and
`host-async` against vdev. `test/async_probe[.exe] ENDPOINT` exercises typed async
model construction, 1 kHz pulled data, mappings, keepalives/watchdog and DISARM;
a serial endpoint also exercises Boot and automatic complete model reload.
Use only a clean loops-only bare bench device with idle groups/capture and a
1 kHz loop. It restores groups and native test rates. On vdev, 1 kHz data is
tested disarmed; arming uses 10 Hz for every native loop and a 1 s command
watchdog to allow desktop scheduling jitter. All original rates/watchdog values
are restored and read back. The physical board retains its timing for both phases.
Linux ThreadSanitizer instructions
are in [development.md](../docs/development.md#host-concurrency-and-time-checks).

Both host validation logs (`build/validate/host-win.log` and `host-linux.log`)
include the integration test's stdout and PASS lines. Validation runs the
other CTest tests normally, then `host-integration` once with `-V`.
To inspect it manually from `host/superbuild`, run
`ctest --preset windows-mingw -V -R '^host-integration$' --no-tests=error`
(use preset `linux` on Linux).

Manual Python fallback, from the repository root in PowerShell: use absolute
executable paths and keep Qt's MinGW runtime on PATH in the same shell:

```powershell
$env:PATH = 'C:\Qt\Tools\mingw1310_64\bin;' + $env:PATH
$cli = (Resolve-Path 'host/superbuild/build/windows-mingw/cli/drivelink-cli.exe').Path
$vdev = (Resolve-Path 'host/superbuild/build/windows-mingw/vdev/drivelink-vdev.exe').Path
python host/test/integration.py --cli $cli --vdev $vdev
```

On Linux, for a preset build from the repository root:
`python3 host/test/integration.py --cli "$(realpath host/superbuild/build/linux/cli/drivelink-cli)" --vdev "$(realpath host/superbuild/build/linux/vdev/drivelink-vdev)"`.
For a WSL build outside the repo, use its absolute build path instead.

By hand in WSL, use a build directory on the Linux filesystem, because
extracting dependencies on `/mnt/c` meets Windows rename locks:
`cmake --preset linux -B ~/.cache/drivelink/build`. Do not use `/tmp`: WSL
keeps it in RAM and loses it when the idle VM stops.

## Try the virtual device

```sh
build/linux/vdev/drivelink-vdev --port 8765 --state /tmp/my-vdev
build/linux/cli/drivelink-cli --endpoint tcp://127.0.0.1:8765 info
build/linux/cli/drivelink-cli --endpoint tcp://127.0.0.1:8765 --json types
```

See [CLI commands](cli/README.md), [vdev behavior](vdev/README.md), and
[the library API](libdrivelink/README.md). The integration tests use separate
processes and temporary state; no ESP32 is needed. On Linux they also test
the serial transport through a raw PTY.

## Bare-board CLI check

Flash normal main firmware, then run from the repository root:

```powershell
python host/test/hardware_smoke.py --cli host/superbuild/build/windows-mingw/cli/drivelink-cli.exe --port COM3
```

This requires a bare ESP32-S3 with UART0 on COM3 and console on COM4. It refuses
pending edits, dirty configuration, invalid layout or safe mode, creates temporary
mock instances, checks persistence/actions/status/diag and restores the original
layout, loop rates and system status rate. It creates no real motor or IMU instance. Do not use
this smoke script with motors or sensors attached.


F4b uses core v5 for arming, fast DISARM and read-only runtime limits. The CLI
adds bounded command/keepalive runs and CSV subscriptions; see cli/README.md.
The library exposes arm/disarm/acknowledgeFault wrappers, and Session routes
system DISARM over MSG_TYPE 0x03. Command values stay explicit and finite.

## Motor controller smoke (F5a-1)

The CLI discovers `controller.cascade_pid` and its live tuning, measurements and
setpoints directly from the device. It supports open-loop duty, velocity PID and
position/velocity cascade. An armed mode change requires a fresh value command;
keepalives preserve the watchdog but leave `waiting_for_command` set.

For a clean loops-only device with idle groups, run:

```powershell
python host/test/motor_smoke.py --cli host/superbuild/build/windows-mingw/cli/drivelink-cli.exe --endpoint serial://COM3 --loop-rate 1000
```

Replace the endpoint with the vdev TCP URL to exercise the identical controller.
The script checks SAVE/reload, velocity and position tracking, mode freshness,
watchdog stop and re-arm, then restores layout and settings. `--report` writes
sample traces and `--stack-wait 35` holds the mock layout for console stack
sampling.

`--modules real` uses `actuator.pwm_dir` and `feedback.quadrature` over serial
only (the vdev has no pins), with `--pwm-pin`, `--dir-pin`, `--enc-a`,
`--enc-b`, scaling (`--cpr`, `--gear`), polarity (`--invert-motor`,
`--invert-encoder`, `--no-pullup`), `--duty-cap` (default 0.2) and tuning,
target and tolerance options. On the bare bench board:

```powershell
python host/test/motor_smoke.py --cli host/superbuild/build/windows-mingw/cli/drivelink-cli.exe --modules real --no-motor --pool-check --pwm-pin 4 --dir-pin 5 --enc-a 6 --enc-b 7
```

checks start on real pins, the hardware force through arm, disarm and the
watchdog, the duty cap and a still count, then that a 5th encoder and a 7th
actuator fail start and ARM is refused. Without `--no-motor` it runs a motor
and needs `--confirm-first-run` (wheels off the ground, a current-limited
supply, a low duty cap, a hand on the power switch). It checks the encoder
polarity in open loop before any closed loop.

`--stall` (with any form; `--stall-ms` 500-4000, default 2000) stalls the
control loop while it drives. It requires Fault/LoopStalled before the stall
can have ended (judged from device data: the loop has not yet counted the
stall's overruns), zero duty (and `forced`
with real modules) after it, and ACK_FAULT and re-arm. Those signals come from
the loop, so only a motor's encoder shows the monitor's force during the
stall; the native tests prove the force without a tick. It needs a
`CONFIG_CORE_BENCH_CRASH=y` firmware (flash the normal one afterwards) or a
vdev started with `--allow-crash`, and refuses before any change otherwise.
Host integration runs the mock form on a vdev with `--allow-crash`.

Linux integration restarts the vdev on a serial PTY (with `--allow-crash`) for
the motor smoke. Buffered
status frames can survive between CLI sessions, so the motor smoke polls for
Disarmed plus the Watchdog reason rather than treating its first queued status
as current. Native tests verify the exact watchdog deadline. On the CH343, a
CLI command or stream operation takes hundreds of milliseconds; the smoke feeds
before mode writes and reuses its held-position sample to avoid an unintended
watchdog gap. It never raises the firmware watchdog beyond its existing range.

## IMU smoke (F6)

`python host/test/imu_smoke.py --cli <cli> --endpoint <endpoint>` exercises
`imu.mock` discovery, typed signal groups, paused calibration actions, rejection,
SAVE/reboot restore and modes; integration runs it on vdev. It requires and
restores a clean loops-only layout with idle groups. `--modules real
--missing-sensor --sda 8 --scl 9` checks the bare board's failed start. With a
BNO055 attached, use `--modules real --sda 8 --scl 9 --calibration-wait 300`;
move the sensor as prompted until all four levels reach 3. Exact profile
comparisons run in CONFIG to avoid automatic recalibration. See
`docs/protocol/imu.md` for the blob, timing and quality contract. Windows uses
the same Qt MinGW PATH as other host binaries.

## Capture smoke (F7a)

`python host/test/capture_smoke.py --cli <cli> --endpoint <endpoint>
[--loop-rate 1000] [--expect-psram]` adds three `actuator.mock` instances to
the control loop and checks a step response captured around a threshold
trigger on the commanded duty, a manual capture forced with `capture-trigger`
at a divider (with the CaptureDone event), ABORT and the WrongState refusals,
and the longest control tick with a 1-ref and a 16-ref capture. Sample spacing
is proven from the captured loop tick counter. Integration runs it on vdev at
200 Hz; on the bare board use `--endpoint serial://COM3 --expect-psram`. It
requires and restores a clean loops-only layout with idle groups.

## Test generator smoke (F7b)

`python host/test/testgen_smoke.py --cli <cli> --endpoint <endpoint>
--loop-rate 1000 [--flash-write] [--report result.json]` captures a generated
step and linear chirp through the cascade controller and mock motor, checks
STOP/hold/fresh-command handoff and watchdog cancellation, and restores the
original loops-only layout/settings. Device loop counters prove timing;
Windows/Linux vdev integration uses 50 Hz for desktop scheduling. Native
tests cover exact waveform/arming/concurrency boundaries. A local frame proxy
drops successful ARM replies and requires cleanup DISARM from both CLI run
workflows. `--flash-write` measures disarmed SAVE during capture for XiP
comparison; `--stack-wait` supports longer log-chirp/stack observation.

With a motor/encoder attached, use `--modules real --confirm-first-run
--pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G]
[--duty-cap 0.2 --probe-duty 0.1 --min-response R]`. First-run procedure:
wheels raised, a current-limited supply, a low duty cap and a hand on the
power switch. A positive step response is required before the chirp.
This real-motor check remains deferred; the same workflow passed with mocks
on vdev and the bare board. XiP measurements and commands are recorded in
`plans/archive/f7b-implementation.md`; normal firmware keeps XiP disabled.

## OTA (F9)

The device must be disarmed and already use the F9 dual-slot table. A board on
F8 needs one UART migration through `python tools/bench_fw.py normal`, retaining
NVS. Build a distinct normal-config candidate without flashing it:

```sh
python tools/bench_fw.py ota-image
drivelink-cli --endpoint serial://COM3 ota-upload firmware/build_ota/drivelink_v2.bin
drivelink-cli --endpoint serial://COM3 ota-status
```

Normal upload preflights/hashes the unsigned ESP32-S3 app, streams bounded
chunks, prepares it with FINISH, requests the explicit REBOOT commit, reconnects,
checks GET_INFO/core v9 and full ELF identity, and confirms that image. Progress
uses stderr; `--json` stdout retains JSON envelopes. `--window 1..4` is bounded
by the device's advertised limit and chunk size fits its payload cap.

`--upload-only` stops in Ready with boot selection untouched; `ota-abort`
cancels it. `--no-confirm` boots the candidate but leaves it pending; a reset
without `ota-confirm` rolls back. ABORT refuses an already selected/pending
image. An accepted-but-uncommitted reboot is reported for STATUS-based recovery,
never automatically repeated. SDK `Client::otaUpload` only uploads/prepares;
`drivelink/ota.hpp` holds the rest of the workflow (`requestOtaReboot`,
`classifyOtaBoot`, `confirmOtaCandidate`) and `otaUpdate`, the synchronous
whole workflow that `ota-upload` runs. The GUI uses the same pieces on its own
connection ([GUI firmware update](gui/README.md#firmware-update-h2b-2)). Low-level
`ota-begin SIZE SHA256_HEX`, `ota-write OFFSET CHUNK_HEX` and `ota-finish` expose
the same contract for diagnostics. See [OTA wire/recovery](../docs/protocol/ota.md).

Bare-board regression (normal firmware; no motors/IMU):

```sh
python host/test/ota_smoke.py --cli <cli> --endpoint serial://COM3 --baseline firmware/build/drivelink_v2.bin --candidate firmware/build_ota/drivelink_v2.bin --stack-wait 45
```

It checks retried/conflicting/gapped chunks, incomplete/hash-invalid FINISH,
partial reset, prepared cancellation, confirmed boot, unconfirmed rollback,
NVS settings and baseline restoration. `--stack-wait` lets the normal console
logger capture the OTA config stack before reset. Host integration runs the
same campaign on vdev with non-executable fixtures; those fixtures are never
for hardware. TCP proxies cover lost replies, identity mismatch, an accepted
but uncommitted reboot, and pipelined commands crossing the commit fence.
