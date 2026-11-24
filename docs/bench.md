# Bench board

Reference for any step that runs a check on the bench board. The board's current
firmware and saved state are in [host-status.md](../plans/host-status.md)
("Board state"); open deferred checks are there too, and results go to
[hardware-log.md](../plans/hardware-log.md). Build, flash and bench-build
commands are in [development.md](development.md).

## The board

- A bare ESP32-S3 (QFN56 rev 0.2, 8 MB PSRAM, 16 MB flash) with the dual-slot
  OTA partition table. No motor and no BNO055 are attached. Real IMU
  missing-sensor checks fail cleanly; mock IMU checks use explicit `imu.mock`
  instances.
- **Never run on hardware so far:** a real BNO055, motors, a board host other
  than Windows. F4b armed only mock actuators on the bare board. These are the
  deferred checks in the status file.

## Ports on this PC

- **COM3** is the CH343 adapter on UART0 (GPIO43/44). It carries the protocol and
  also flashes the board through DTR/RTS.
- **COM4** is the native USB port, the console.

## The board through the agent (WSL)

`drivelink-agent` is Linux only. To put the board behind it, lend COM3's
adapter to WSL with `tools/wsl_serial.py` (one-time setup: `usbipd bind` in an
admin shell and the `dialout` group in WSL; see
[development.md](development.md), "Serial adapter in WSL"). `run` attaches it
(it shows up as `/dev/ttyACM0`), runs the command and always gives it back:

```
python tools/wsl_serial.py run -- wsl.exe --exec bash -c 'drivelink-agent --port 47620 --token <token> --device "bench=serial://$DRIVELINK_WSL_TTY@921600"'
```

Windows reaches the agent at `127.0.0.1:47620` (WSL forwards localhost).
While it is attached, COM3 is gone from Windows. Stop the agent (Ctrl+C) to
give the adapter back before flashing or a direct check. The GUI's PC check
`gui_remote_tests benchRemote` (`DRIVELINK_GUI_BENCH_AGENT=127.0.0.1:47620`,
`_TOKEN`, `_DEVICE=bench`, `_CLI=<drivelink-cli.exe>`) runs against it and
writes nothing to the board. Over serial the agent keeps the port across a
REBOOT, so its Boot path runs here (a TCP vdev looks like device loss).

### The agent package in WSL (H3c)

The Ubuntu-26.04 distro runs systemd, so the packaged agent runs there as a
service. `host/test/deploy_check.py` installs the `build/deb/` packages (after
the `deb` check). It adds the board through the udev name and
`drivelink-agent --list-ports`, checks that an upgrade leaves the service's
MainPID alone, then removes and purges. It needs root, given with
`wsl -u root`; the user approved that for this check on 2026-11-20. Run it
through `wsl_serial.py run`, which attaches the CH343 for the run and always
gives it back:

```
python tools/wsl_serial.py run -- wsl.exe -d Ubuntu-26.04 -u root --cd <repo> --exec python3 host/test/deploy_check.py --debs build/deb --phase all
```

`--endpoint vdev` is the same run without the board (the installed vdev as
the device). The CH343 shows up as `/dev/drivelink/<USB serial>` and also as
`/dev/ttyACM0`. It only reads (`info`, `status`); the agent DISARMs the board
when it opens it and when it stops. The check needs a distro without
`drivelink-agent` installed and leaves none behind.

### Finding the agent (mDNS)

An agent bound to a LAN address advertises itself (`_drivelink._tcp`); one
bound to `127.0.0.1`, as above, never does. In WSL that needs mirrored
networking (`networkingMode=Mirrored` in `%USERPROFILE%\.wslconfig`, then
`wsl --shutdown`), so WSL holds the PC's LAN address; the agent then shares
UDP 5353 with Windows' own mDNS. `drivelink-cli agent-browse` inside WSL finds
it, and so do other machines on the LAN, but the GUI on the same PC cannot:
Windows' own multicast never reaches mirrored WSL, and Windows reaches the
agent's LAN address only with `hostAddressLoopback=true` and a Hyper-V
firewall rule. On one PC, bind the agent to `127.0.0.1` and type
`127.0.0.1:47620`. Mirrored mode keeps the localhost
forwarding `benchRemote` uses. `gui_remote_tests benchDiscovery`
(`DRIVELINK_GUI_BENCH_DISCOVER=<instance>`, `_TOKEN`, `_DEVICE`) is the check
for a GUI on another machine than the agent.

## Serial traps

- **First open resets the board.** The first time the adapter's port is opened
  after a flash, the board resets and the first command is lost.
  `smoke_protocol_firmware.py` retries its first query for this reason.
- **Windows serial in `libdrivelink`.** Asio's `flow_control(none)` reasserts
  DTR/RTS and resets the board, so keep the bootstrap lines that `open()` leaves
  disabled. A native Win32 `AccessDenied` or `FileNotFound` at reopen is retried
  before any request is sent.
- **Status backlog.** On Windows over the CH343, each new CLI process reads queued
  status frames, not the newest one, so a check that polls `status` sees device
  events late. Judge timing from device data (timestamps, overrun counts), as
  `motor_smoke.py --stall` does. Never time anything with host clocks over the
  CH343.
- **Console and reboots.** Close a COM4 console capture before a cleanup reboot.
  Native USB control-line transitions can reset the ESP (reset reason 11,
  `ESP_RST_USB`). Configure DTR/RTS inactive before opening; after closing,
  establish readiness with GET_INFO before cleanup mutations. The affinity
  smoke does this explicitly; it never retries an uncertain mutation.
- The host PC bugchecked (0xD1) once during an F5a-2 run on the CH343; the rerun
  passed. Rerun once before suspecting the firmware.

## Smoke scripts

- `firmware/test/uart_comm/smoke_protocol_firmware.py`: diag v3 and time v1 on the
  main firmware. It creates no instance.
- `firmware/test/uart_comm/smoke_core_firmware.py`: the core with mock instances,
  which it removes again. `--safe-mode` needs a `CONFIG_CORE_BENCH_CRASH=y` build;
  flash the normal firmware again afterwards.
- `host/test/hardware_smoke.py`: the same through the CLI. It refuses to run with
  pending edits, and restores the layout and rates it found.
- The module smokes (`motor_smoke.py`, `imu_smoke.py`, `capture_smoke.py`,
  `testgen_smoke.py`, `ota_smoke.py`, `loop_affinity_smoke.py`, `time_smoke.py`)
  are described in [host/README.md](../host/README.md). Every one restores the
  layout and settings it found.

## Bench builds

Use `python tools/bench_fw.py bench|normal|xip|ota-image` (see
[development.md](development.md#guarded-bench-builds)), never an edited normal
sdkconfig. Always restore `normal` afterwards and check that the board refuses
bench commands.

## Pins for real modules

Actuator PWM 4 / DIR 5, encoder 6 / 7 (nothing attached; pull-ups keep the count
at 0). `motor_smoke.py`'s pool and frequency checks use 1, 2, 10–18, 21, 39–42 and 47. Avoid
the strapping pins 0, 3, 45, 46, the example IMU's I2C 8/9, and 38/48 (RGB LED
on some boards).

## Stack rule

Every task keeps more than 1 KB free. Check the marks after adding a task or a
large buffer. `app_main`'s locals are large, so look at `main` after adding
anything it starts. Stack sizes: main 5632, core_cfg 6144, others 4096.

Marks with 6 loops, 12 + 12 groups and 20 mock modules (runtime settings step 2,
normal build, console): main 3552, uart_rx 2996, uart_tx 3056, diag 1728,
esp_timer 3264, core_cfg 3952 (2624 after an edit and SAVE), loops 2908 to 3080.

Marks with six actuators at six PWM frequencies, or a 5th encoder or 7th actuator
failing start (runtime settings step 3, normal build, console): main 2544,
uart_rx 2756, uart_tx 3056, diag 1788, esp_timer 3088, core_cfg 4016, loops 3064.

Latest marks (H1c normal firmware, minimum across maximum-name and affinity
observations, bytes free): main 3552, uart_rx 3028, uart_tx 3056, diag 1752,
esp_timer 3264, core_cfg 2632, core_loop_0 3136, core_loop_1 3252, temporary
core_loop_2 3168, core_loop_3 3192. F10's deeper
OTA/safety hold reached core_cfg 2064 and diag 1628. There is no OTA or time
task.
