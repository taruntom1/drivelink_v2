# mod_mock

Pure, deterministic module types for native tests, the [virtual device](../../../host/vdev/README.md) and bare-board firmware checks. [`mod_mock::kTypes`](include/mod_mock/modules.hpp) is the explicit registration list; the logic is in [`src/modules.cpp`](src/modules.cpp). Ticks and actions never allocate, block or log.

Descriptors, IDs, allowed loops, params and defaults, signals, inputs and actions are generated from the YAML files:

| Type | Generated reference | Definition |
|---|---|---|
| `actuator.mock` | [actuator.mock.md](../../../docs/protocol/types/actuator.mock.md) | [actuator.mock.yaml](../../../protocol/defs/actuator.mock.yaml) |
| `feedback.mock` | [feedback.mock.md](../../../docs/protocol/types/feedback.mock.md) | [feedback.mock.yaml](../../../protocol/defs/feedback.mock.yaml) |
| `imu.mock` | [imu.mock.md](../../../docs/protocol/types/imu.mock.md) | [imu.mock.yaml](../../../protocol/defs/imu.mock.yaml) |
| `fusion.mock` | [fusion.mock.md](../../../docs/protocol/types/fusion.mock.md) | [fusion.mock.yaml](../../../protocol/defs/fusion.mock.yaml) |

Native command tests and [`smoke_core_firmware.py`](../../test/uart_comm/smoke_core_firmware.py) exercise these types. Command inputs and arming work as for any module.

## actuator.mock

A first-order velocity plant with integrated position, driven by duty through `tau_s` and `gain`. It implements the `mod_motor` actuator interface.

- Each tick clamps the requested duty to `duty_limit`, zeroes it inside `deadband`, then applies it directly or, with `one_tick_delay`, one tick later. Feedback sampling the previous tick adds a further tick.
- Emergency force or the disarmed state zeroes both requested and queued duty; the plant then coasts. Every arm transition clears duty, queued duty and applied duty, so disarmed `drive` actions leave zero target duty.
- `applied_duty` is the plant's effort after limit, deadband and delay, not a controller's commanded duty.
- `drive` is a simulation test control: it sets the standalone duty and returns velocity and position. Once a controller claims the actuator at startup, the duty input has no effect and `drive` returns `Busy/AlreadyBound`.
- Paused `reset` zeroes the plant, advances its reset generation and echoes its blob, exercising bounded bidirectional action payloads.
- The `diagnostic(disarms, active)` loop action raises or clears a mock arm blocker (`disarms` false) or a disarming latched fault (`disarms` true), so the real safety path can be tested without motors.

## feedback.mock

Simulated encoder feedback implementing the `mod_motor` feedback interface.

- Its optional `actuator` reference supplies a plant observation, sampled from the previous tick before the actuator ticks. A referenced type without plant observation fails start at stage 1 (simulation source).
- Position is quantized to a signed int32 `count` with `counts_per_rev` and `gear_ratio`; position and filtered velocity come from that count through the hardware feedback estimator. The count wraps, the velocity delta handles the wrap, and float position wraps with the count. Use raw counts for odometry.
- `noise_counts` adds bounded count noise before rounding from a deterministic xorshift seeded by `noise_seed`. A plant reset or a seed change clears the estimator and restarts the sequence.
- Arm transitions re-prime the estimator without clearing the count.

## imu.mock

Constant-rate yaw wrapped to ±π, with a matching quaternion and representative gyro, accel, gravity, magnetometer and temperature signals, plus `sample` and paused `reset` actions. It uses the [`mod_imu_bno055`](../mod_imu_bno055/README.md) state, modes and calibration codec, so calibration actions behave as in the [IMU contract](../../../docs/protocol/imu.md#calibration-actions-and-persistence). Mode 0 (CONFIG) publishes no valid samples.

All four calibration levels report 3 only while a valid calibration is applied, and 0 otherwise. A saved calibration applies at start. On a fresh device, write and apply (or save and reboot with) a calibration before a check that needs levels at 3, such as the GUI's Read.

## fusion.mock

Two [sources](../../../docs/protocol/core.md#sources-f4c), `feedback` and `imu`, with their generated value and age signals, and `combined`, the sum of the valid source values (invalid sources contribute zero). It runs on any loop and exercises same-loop and cross-loop sampling without an actuator output.

## Controllers

A controller claims its actuator at startup. Duplicate controllers fail startup and block ARM. A mock without a controller keeps its standalone input and action API. See [Wiring and ownership](../mod_motor/README.md#wiring-and-ownership) for interface lifetime and emergency force.
