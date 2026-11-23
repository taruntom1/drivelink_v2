# DriveLink host tools

| Component | Responsibility |
|---|---|
| [libdrivelink](libdrivelink/README.md) | Typed C++ client sessions, service calls and discovered device state. |
| [CLI](cli/README.md) | Command-line access to a device over serial, TCP or an agent connection. |
| [Virtual device](vdev/README.md) | Runs the firmware core and simulated modules for host integration tests. |
| [Desktop GUI](gui/README.md) | Operator interface with direct serial/TCP and remote agent connections. |
| [Linux agent](agent/README.md) | Owns device links and arbitrates command leases across clients. |
| [ROS hardware plugin](ros/drivelink_hardware/README.md) | Maps ros2_control state and commands through the agent. |
| [ROS bringup](ros/drivelink_bringup/README.md) | Launch files, controller configuration, diagnostics and recovery for the example robot. |

## Build

The [superbuild presets](superbuild/CMakePresets.json) include `linux` and `windows-mingw` for GUI builds. To build and test on Linux from the repository root:

```sh
cd host/superbuild
cmake --preset linux
cmake --build --preset linux --parallel
ctest --preset linux
```

For headless Linux, configure with `cmake --preset linux-no-gui` in the superbuild and build with `cmake --build build/linux-no-gui --parallel`.

Preset binaries are under `host/superbuild/build/<preset>/` in component directories such as `cli/`, `vdev/` and `gui/`. For a manual WSL build, choose a persistent Linux-filesystem build directory to avoid dependency extraction locks on mounted Windows volumes. See the [development guide](../docs/development.md#commands) for toolchain setup, the [validation matrix](../docs/development.md#validation-toolsvalidatepy) for host and installation checks, and the [released package guide](https://taruntom1.github.io/drivelink_v2/reference/packages/) for installation.

## Verification

The automated host checks exercise discovery, identity, concurrency and transport through vdev. See the [library checks](../docs/development.md#library-checks) for probe commands and serial prerequisites. The read-only `model_probe ENDPOINT` loads and reloads the device snapshot. For integration failures, run `ctest --preset linux -V -R '^host-integration$' --no-tests=error` from the superbuild, substituting the Windows preset as needed.

Run the following physical checks from the repository root. Port examples refer to the [bench configuration](../docs/bench.md). For the identity check, use a disarmed device on normal firmware with saved settings and idle groups. The script creates a temporary mock actuator and restores name, layout and settings.

```sh
python host/test/identity_smoke.py --cli <cli> --endpoint serial://COM3 --probe <async_probe>
```

## Bare-board CLI check

Flash normal main firmware, then run from the repository root:

```powershell
python host/test/hardware_smoke.py --cli host/superbuild/build/windows-mingw/cli/drivelink-cli.exe --port COM3
```

This requires a bare ESP32-S3 with UART0 on COM3 and console on COM4. It refuses pending edits, dirty configuration, invalid layout or safe mode, creates temporary mock instances, checks persistence/actions/status/diag and restores the original layout, loop rates and system status rate. It creates no real motor or IMU instance. Do not use this smoke script with motors or sensors attached.

## Motor controller smoke (F5a-1)

For a clean loops-only device with idle groups, run:

```powershell
python host/test/motor_smoke.py --cli host/superbuild/build/windows-mingw/cli/drivelink-cli.exe --endpoint serial://COM3 --loop-rate 1000
```

Replace the endpoint with the vdev TCP URL to exercise the identical controller. The script checks SAVE/reload, velocity and position tracking, mode freshness, watchdog stop and re-arm, then restores layout and settings. `--report` writes sample traces and `--stack-wait 35` holds the mock layout for console stack sampling.

`--modules real` uses `actuator.pwm_dir` and `feedback.quadrature` over serial only (the vdev has no pins), with `--pwm-pin`, `--dir-pin`, `--enc-a`, `--enc-b`, scaling (`--cpr`, `--gear`), polarity (`--invert-motor`, `--invert-encoder`, `--no-pullup`), `--duty-cap` (default 0.2) and tuning, target and tolerance options. On the bare bench board:

```powershell
python host/test/motor_smoke.py --cli host/superbuild/build/windows-mingw/cli/drivelink-cli.exe --modules real --no-motor --pool-check --pwm-pin 4 --dir-pin 5 --enc-a 6 --enc-b 7
```

checks start on real pins, the hardware force through arm, disarm and the watchdog, the duty cap and a still count, then that a 5th encoder and a 7th actuator fail start and ARM is refused. Without `--no-motor` it runs a motor and needs `--confirm-first-run` (wheels off the ground, a current-limited supply, a low duty cap, a hand on the power switch). It checks the encoder polarity in open loop before any closed loop.

`--stall` (with any form; `--stall-ms` 500-4000, default 2000) stalls the control loop while it drives. It requires Fault/LoopStalled before the stall can have ended (judged from device data: the loop has not yet counted the stall's overruns), zero duty (and `forced` with real modules) after it, and ACK_FAULT and re-arm. Those signals come from the loop, so only a motor's encoder shows the monitor's force during the stall; the native tests prove the force without a tick. It needs a `CONFIG_CORE_BENCH_CRASH=y` firmware (flash the normal one afterwards) or a vdev started with `--allow-crash`, and refuses before any change otherwise. Host integration runs the mock form on a vdev with `--allow-crash`.

Linux integration restarts the vdev on a serial PTY (with `--allow-crash`) for the motor smoke. Buffered status frames can survive between CLI sessions, so the motor smoke polls for Disarmed plus the Watchdog reason rather than treating its first queued status as current. Native tests verify the exact watchdog deadline. On the CH343, a CLI command or stream operation takes hundreds of milliseconds; the smoke feeds before mode writes and reuses its held-position sample to avoid an unintended watchdog gap. It never raises the firmware watchdog beyond its existing range.

## IMU smoke (F6)

`python host/test/imu_smoke.py --cli <cli> --endpoint <endpoint>` exercises `imu.mock` discovery, typed signal groups, paused calibration actions, rejection, SAVE/reboot restore and modes; integration runs it on vdev. It requires and restores a clean loops-only layout with idle groups. `--modules real --missing-sensor --sda 8 --scl 9` checks the bare board's failed start. With a BNO055 attached, use `--modules real --sda 8 --scl 9 --calibration-wait 300`; move the sensor as prompted until all four levels reach 3. Exact profile comparisons run in CONFIG to avoid automatic recalibration. See the [IMU contract](../docs/protocol/imu.md) for profile encoding and timing.

## Capture smoke (F7a)

`python host/test/capture_smoke.py --cli <cli> --endpoint <endpoint> [--loop-rate 1000] [--expect-psram]` adds three `actuator.mock` instances to the control loop and checks a step response captured around a threshold trigger on the commanded duty, a manual capture forced with `capture-trigger` at a divider (with the CaptureDone event), ABORT and the WrongState refusals, and the longest control tick with a 1-ref and a 16-ref capture. Sample spacing is proven from the captured loop tick counter. Integration runs it on vdev at 200 Hz; on the bare board use `--endpoint serial://COM3 --expect-psram`. It requires and restores a clean loops-only layout with idle groups.

## Test generator smoke (F7b)

`python host/test/testgen_smoke.py --cli <cli> --endpoint <endpoint> --loop-rate 1000 [--flash-write] [--report result.json]` captures a generated step and linear chirp through the cascade controller and mock motor, checks STOP/hold/fresh-command handoff and watchdog cancellation, and restores the original loops-only layout/settings. Device loop counters prove timing; Windows/Linux vdev integration uses 50 Hz for desktop scheduling. Native tests cover exact waveform/arming/concurrency boundaries. A local frame proxy drops successful ARM replies and requires cleanup DISARM from both CLI run workflows. `--flash-write` measures disarmed SAVE during capture for XiP comparison; `--stack-wait` supports longer log-chirp/stack observation.

With a motor/encoder attached, use `--modules real --confirm-first-run --pwm-pin P --dir-pin D --enc-a A --enc-b B --cpr C [--gear G] [--duty-cap 0.2 --probe-duty 0.1 --min-response R]`. First-run procedure: wheels raised, a current-limited supply, a low duty cap and a hand on the power switch. A positive step response is required before the chirp. See [hardware status](../plans/host-status.md#deferred-hardware-checks) for outstanding checks and the [XiP measurements](../plans/archive/f7b-implementation.md) for the bench campaign.

## OTA (F9)

The device must be disarmed and already use the F9 dual-slot table. A board on F8 needs one UART migration through `python tools/bench_fw.py normal`, retaining NVS. Build a distinct normal-config candidate without flashing it:

```sh
python tools/bench_fw.py ota-image
drivelink-cli --endpoint serial://COM3 ota-upload firmware/build_ota/drivelink_v2.bin
drivelink-cli --endpoint serial://COM3 ota-status
```

After upload and reboot, verify the running image identity before confirmation. For a prepared image (`--upload-only`), use `ota-abort` to cancel before boot selection. With `--no-confirm`, reset the unconfirmed candidate to roll back. If reboot acceptance is uncertain, read STATUS and reconcile the running image and slot before acting. Never automatically repeat REBOOT. See the [OTA recovery contract](../docs/protocol/ota.md) and [CLI commands](cli/README.md).

Bare-board regression (normal firmware; no motors/IMU):

```sh
python host/test/ota_smoke.py --cli <cli> --endpoint serial://COM3 --baseline firmware/build/drivelink_v2.bin --candidate firmware/build_ota/drivelink_v2.bin --stack-wait 45
```

It checks retried/conflicting/gapped chunks, incomplete/hash-invalid FINISH, partial reset, prepared cancellation, confirmed boot, unconfirmed rollback, NVS settings and baseline restoration. `--stack-wait` lets the normal console logger capture the OTA config stack before reset. Host integration runs the same campaign on vdev with non-executable fixtures; those fixtures are never for hardware. TCP proxies cover lost replies, identity mismatch, an accepted but uncommitted reboot, and pipelined commands crossing the commit fence.
