# mod_imu_bno055

The `imu.bno055` core module: an instance-owned pure register driver over a
small bus interface, with an ESP-IDF master-I2C backend. It replaces the old
IMU manager and Bosch global driver context. The firmware registers `kType`;
vdev uses `imu.mock` and the shared state/calibration codec.

## API and ownership

- `modules.hpp`: the explicit module factory/descriptor `kType`.
- `bus.hpp`: `Bus`, `BusConfig`, setup-only `openBus`, and pure configuration
  bounds. Read/write return `core_os::Err`; delays guarantee a minimum.
- `driver.hpp`: setup, mode, sample and calibration register operations.
- `state.hpp`: shared sample bindings, availability and exact calibration codec.

The type is forbidden in real-time loops. Boot binds all storage, takes an
exclusive SDA/SCL pair and port, checks identity/configuration/health, applies
saved calibration, then selects the startup mode. A mutex protects the two-port
pool only during setup/release; runtime access belongs to the owning loop or
the config caller after a paused-action acknowledgement. Failure releases
acquired resources; there is no silent mock fallback or dedicated task.

Pins, port, address, clock, timeout and startup mode are discovered per-instance
parameters. Kconfig has no component-global tunables. Clock bounds are
100–400 kHz and requested timeouts 10–20 ms. IDF waits round to 20–30 ms at
FreeRTOS 100 Hz; a transfer can span several waits. Use the factory 100 Hz
sensor loop as the default. Bus delays can still trigger the unchanged safety
monitor; do not add a monitor exemption. Use suitable board wiring/pull-ups for
the chosen I2C speed.

Ticks allocate nothing. One 54-byte burst is decoded into temporary storage;
only complete, healthy data replaces the sample. Mode/calibration actions are
paused and have bounded bus waits/readiness polling. Signals, coordinate
convention, readiness/recovery and calibration wire/persistence semantics are
in [the IMU contract](../../../docs/protocol/imu.md).

## Checks

Native `test_imu_modules.cpp` and `fake_imu_bus.cpp` cover the register sequence,
settling, decoding, health/reset/unit failures, supported bus bounds, pool
ownership, calibration errors and transaction/persistence lifecycle.
`python tools/validate.py full` runs these plus host integration and IDF builds.

The same host script exercises mock and real modules:

```powershell
python host/test/imu_smoke.py --cli <cli> --endpoint serial://COM3
python host/test/imu_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --missing-sensor --sda 8 --scl 9
python host/test/imu_smoke.py --cli <cli> --endpoint serial://COM3 --modules real --sda 8 --scl 9 --address 0x28 --calibration-wait 300
```

The real run waits for calibration levels 3/3/3/3, then compares hardware
profile application and boot restore in CONFIG before resuming NDOF. It requires
a clean loops-only layout, restores it, and never applies synthetic calibration
to real hardware. Physical calibration quality remains a deferred HW check.
