# mod_imu_bno055

| Source | Responsibility |
|---|---|
| [Module lifecycle](src/module.cpp) | Binds instance storage, restores calibration, publishes samples and dispatches paused actions. |
| [Register driver](src/driver.cpp) | Implements sensor initialization, mode transitions, burst reads and calibration over the [bus interface](include/mod_imu_bno055/bus.hpp). |
| [Sample state and calibration codec](src/state.cpp) | Shared by the physical module and the virtual device’s [mock IMU](../mod_mock/src/modules.cpp). |
| [ESP-IDF bus](src/idf/bus_idf.cpp) | Owns master-I²C handles, port claims, transfer timeouts and settling delays. Native module tests substitute a [fake bus](../../test/native/tests/fake_imu_bus.cpp). |

## Bus ownership and timing

Each instance binds its storage before claiming its configured port and pins. It initializes the device, applies valid saved calibration, then selects the startup mode. The pool mutex protects port acquisition and release. During operation, the non-real-time owning loop accesses the driver on ticks. The configuration caller may access it after the loop acknowledges a paused action.

At the firmware’s 100 Hz FreeRTOS tick rate, the backend rounds a requested 10–20 ms timeout to 20–30 ms per driver wait. A transfer can span several waits, so that value cannot serve as a deadline for the whole tick.

## Sample publication

A tick reads one 54-byte burst into temporary storage. Only a complete, healthy read publishes new measurements, timestamped at the midpoint of the read interval. On failure, the previous measurements remain, validity clears, age advances and the instance raises its read-failure diagnostic. The next successful publication clears that diagnostic. CONFIG mode skips sampling and clears validity and availability.

See the [IMU contract](../../../docs/protocol/imu.md) for signal conventions and calibration persistence, the [native module tests](../../test/native/tests/test_imu_modules.cpp) for register sequencing and fault injection, and the [host smoke procedure](../../../host/README.md#imu-smoke-f6) for hardware checks.
