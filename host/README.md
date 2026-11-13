# DriveLink host tools (F3c)

`libdrivelink` provides serial/TCP sessions and the current core families.
`drivelink-cli` drives the ESP32 or `drivelink-vdev` with identical commands.
The library has no Qt or ROS dependency. Time synchronization, the cached
device model and the rest of H1 are still planned.

## Build

Requires CMake 3.24+, Ninja, a C++20 compiler and Python 3. The first configure
downloads SHA256-pinned standalone Asio 1.30.2 and nlohmann JSON 3.11.3.
The presets do not build a GUI yet.

```sh
cd host/superbuild
cmake --preset linux-no-gui
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

The binaries are under `build/<preset>/cli/` and `build/<preset>/vdev/`.
Keep the same MinGW runtime DLLs on PATH when running them.

From the repository root, `python tools/validate.py --only host-win host-linux`
runs both sequences above with the right toolchain for each, the Linux one
inside WSL (see firmware/test/CLAUDE.md, "Validation").

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
layout, loop rates and system status rate. It never sends the legacy motor commands. Do not use
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
