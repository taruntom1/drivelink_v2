# IMU modules (F6)

`imu.bno055` (type `0x30`) runs in a non-real-time loop, normally the factory
sensor loop at 100 Hz. `imu.mock` (`0x72`) provides the same calibration and
signal contract on vdev and the bare board. Discover items through the ordinary
core describe family; the generated [BNO055](types/imu.bno055.md) and
[mock](types/imu.mock.md) tables hold their IDs. Core stays v5 and diag v3.

## Configuration and signals

The real type has structural SDA/SCL pins, port 0 or 1, address `0x28` or
`0x29`, `bus_hz` 100000–400000 and `timeout_ms` 10–20 (defaults 400000 and
10). Pins start unset (`0xFF`); each instance owns its pins and I2C controller.
There is no shared-pin bus. A missing sensor fails the instance with
`imu_bno055.InitFailed`; that failed instance prevents ARM. There is no mock
fallback. The firmware registers the real type; vdev registers the mock.

`startup_mode` persists and requires SAVE/reboot. Modes are 0 CONFIG, 1 ACCONLY,
2 MAGONLY, 3 GYROONLY, 4 ACCMAG, 5 ACCGYRO, 6 MAGGYRO, 7 AMG, 8 IMU, 9 COMPASS,
10 M4G, 11 NDOF_FMC_OFF and 12 NDOF (default). The paused `set_mode(mode u8)`
action changes the current mode; `actual_mode` reports it. This action leaves
`startup_mode` unchanged. Mode 255 means the driver cannot establish the mode.

Signals include Euler yaw/roll/pitch (rad), quaternion w/x/y/z, linear
acceleration, gravity and accelerometer vectors (m/s2), gyro (rad/s), magnetic
field (uT), temperature (integer degC) and calibration levels sys/gyro/accel/mag
(0–3). The sensor axes/signs are explicitly configured to native X/Y/Z
(`AXIS_MAP_CONFIG=0x24`, signs 0). Euler uses the BNO055 Windows convention:
heading/yaw increases clockwise, pitch is -pi to +pi, roll -pi/2 to +pi/2.
The future ROS adapter must account for the sensor frame and convention.

`available` identifies signal families meaningful in the actual mode:

| Bit | Family |
|---|---|
| 0 | Euler |
| 1 | Quaternion |
| 2 | Linear acceleration |
| 3 | Gravity |
| 4 | Accelerometer |
| 5 | Gyroscope |
| 6 | Magnetometer |
| 7 | Temperature (accelerometer source, only with accelerometer enabled) |
| 8 | Calibration status |

`valid` requires a complete successful burst, matching mode and units,
SYS_ERR=0, and running SYS_STATUS (5 for fusion, 6 for raw modes). CONFIG
publishes invalid data. `sample_count` advances only on complete success;
`sample_age_us` grows since the last successful sample and saturates at
0xFFFFFFFF (also its value before any sample). Failed reads retain numeric
values, mark invalid and raise the non-disarming `ReadFailed` warning. A
healthy sample clears it. Source-link age measures core publication age;
consumers needing sensor freshness must also use sensor validity/sample age.
Paused actions stop publication from the entire owning loop until they finish.

A sensor-only reset, changed units or internal error cannot publish a fresh
valid sample. Reboot the device to reinitialize a reset sensor and reapply saved
calibration. An explicit mode action can recover an uncertain mode write when
the rest of the sensor configuration is intact. Startup and mode actions wait
for initialization readiness with bounded polling; internal errors fail them.

## Calibration actions and persistence

All three actions run paused:

- `calibration_read`: enter CONFIG, read offsets/radii/SIC, restore mode,
  return the blob and stage it in persistent parameters. Marks NVS dirty.
- `calibration_write`: validate and atomically stage a supplied blob in RAM.
  Marks NVS dirty. It performs no bus write.
- `calibration_apply`: write staged calibration in CONFIG and restore mode.
  Missing staged calibration fails with InvalidArgument/BadValue.

Bus/action failures return Fault/BadValue and report `ActionFailed` (arg0 action
id, arg1 actual mode, esp_err the bus/driver error). A failed read or malformed
write leaves staged calibration unchanged. Mode restoration is attempted after
calibration transfer errors; an uncertain mode is exposed as 255.

The action blob is exactly 42 bytes, little-endian:

`version u8=1 | has_sic u8 (0/1) | 20 * i16`

The twenty values are accel X/Y/Z/radius, gyro X/Y/Z, mag X/Y/Z/radius, then
the nine SIC matrix coefficients in row-major order. Offsets and SIC use signed
16-bit sensor values; radii accept 0–32767. SIC coefficients use the sensor's
Q14 representation. `has_sic=false` leaves the hardware SIC matrix unchanged;
all nine values remain present in the blob. Hardware reads include SIC.
Wrong versions, lengths, flags or negative radii are rejected before mutation.

`calibration.valid` records blob presence. Physical calibration quality is
reported by the four calibration-status levels. The BNO055 profile should be
captured after full calibration. Fusion automatically updates offsets/radii,
so exact hardware comparisons belong in CONFIG with fusion stopped.

The staged fields are ordinary typed persistent parameters (`calibration.*`).
Explicit SAVE commits them with the layout through the existing complete-
generation A/B transaction. Calibration actions never write flash. Boot applies
a saved valid calibration before selecting `startup_mode`; restore failure
fails the instance and prevents ARM. REVERT restores staged values; applying
them to the current sensor requires `calibration_apply` or reboot. Removal and
factory reset use the normal instance-namespace lifecycle. Instance format 1
and layout format 2 are unchanged.

## Timing and retired wire

The sample uses one 54-byte burst (0x08–0x3D). At 100 kHz it needs about 5.1 ms
before stretching or scheduling; 400 kHz is the default. At FreeRTOS 100 Hz,
the requested timeout is rounded up with one extra tick: 20–30 ms per IDF
wait. FIFO progress/recovery can use several waits. Pathological bus delay,
multiple sensors in one loop, or an excessively fast loop can still trigger
the ordinary LoopStalled safety response. The monitor is unchanged.

F6 removes legacy IMU messages `0x22`/`0x42` and stream `0xBF`, and retires the
legacy source's codes while preserving source id `0x12`. Manager id 3 and
started bit `0x04` remain reserved. Command groups now have 32 channels
(`0xA0`–`0xBF`); subscriptions retain 31 (`0x81`–`0x9F`). `0x42` remains free
until the F8 time-ping family.

Reference: [Bosch BNO055 datasheet revision 1.8](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bno055-ds000.pdf),
sections 3.3, 3.6–3.7, 3.11 and 4.3.
