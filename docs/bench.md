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
at 0). `motor_smoke.py`'s pool check uses 1, 2, 10–18, 21, 39–42 and 47. Avoid
the strapping pins 0, 3, 45, 46, the example IMU's I2C 8/9, and 38/48 (RGB LED
on some boards).

## Stack rule

Every task keeps more than 1 KB free. Check the marks after adding a task or a
large buffer. `app_main`'s locals are large, so look at `main` after adding
anything it starts. Stack sizes: main 5632, core_cfg 6144, others 4096.

Latest marks (H1c normal firmware, minimum across maximum-name and affinity
observations, bytes free): main 3552, uart_rx 3028, uart_tx 3056, diag 1752,
esp_timer 3264, core_cfg 2632, core_loop_0 3136, core_loop_1 3252, temporary
core_loop_2 3168, core_loop_3 3192. F10's deeper
OTA/safety hold reached core_cfg 2064 and diag 1628. There is no OTA or time
task.
