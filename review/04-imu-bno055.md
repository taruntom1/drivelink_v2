# 04: IMU (BNO055) Component

---

## I1. Medium: `ReadCalibration` reads offset registers outside CONFIG mode

[bno055_device.cpp:267-321](../components/imu_bmo_055/src/bno055_device.cpp)

The BNO055 datasheet (§3.6.4, "Reading Calibration profile") says to switch to `CONFIG_MODE` before reading the offset and radius registers. The calibration profile is only valid there. In NDOF and the other fusion modes, the read returns whatever the registers held at the last config entry, or default values.

`writeCalibration()` already does the "save mode → CONFIG → write → restore mode" dance. `readCalibration()` does not. The host saves this blob and later writes it back with `ApplyCalibration`, so persisted calibration may be garbage.

**Fix**: put the mode save/restore from `writeCalibration` into a small RAII helper (`ConfigModeScope`) and use it in both functions. Note that a mode switch needs ~7–19 ms of settle time; the Bosch driver already handles that delay.

---

## I2. Medium: silent fallback to random mock data

- [imu_bmo_055.cpp:312-323](../components/imu_bmo_055/src/imu_bmo_055.cpp)
- [Kconfig](../components/imu_bmo_055/Kconfig) `IMU_BMO_055_FALLBACK_TO_MOCK_ON_INIT_FAIL` has `default y`

If the sensor is unplugged or the I²C bus fails at boot, the manager logs a warning and streams `esp_random()` values in the same packet format, with sensible ranges and calibration status 2–3. Nothing in the stream or in the configure responses tells the host this is synthetic. A robot could fuse random orientation data.

**Fix**: default the fallback to `n`. If it is enabled, set a flag bit in every stream packet (there are spare field-mask bits, or add a status byte) and emit the event message type (`IMU_BMO_055_EVENT_MSG_TYPE`, currently unused) once at startup.

---

## I3. Low: uncommitted change compiles the Bosch template `bno055_support.c`

- [CMakeLists.txt:2](../components/imu_bmo_055/CMakeLists.txt) (working tree): `SRCS "bno055_support.c" "bno055.c" …`
- [bno055_support.c:103](../components/imu_bmo_055/bno055_support.c): `struct bno055_t bno055;` (a non-static global)
- [bno055_support.c:319](../components/imu_bmo_055/bno055_support.c): `bno055_init(&bno055);` inside `bno055_data_readout_template()`

`bno055_support.c` is Bosch's *example* file. `BNO055_API` is not defined anywhere, so the I²C stubs are compiled out. What remains is:
- a global `bno055` struct whose `bus_read` / `bus_write` / `delay_msec` are all null, and
- a 200-line demo function that calls `bno055_init(&bno055)`.

`bno055_init` stores its argument in the driver's static `p_bno055`. If anyone ever calls the template, every later IMU read through `Bno055Device` goes through null function pointers and crashes. It also adds dead code and a confusingly named global to the image. And if someone later defines `BNO055_API` to "enable" it, the build breaks: `BNO055_I2C_bus_write` has a stray closing brace at line 579 and an 8-byte stack buffer that overflows for `cnt > 7`.

Nothing in `Bno055Device` needs this file. All bus callbacks are provided by the thunks in `bno055_device.cpp`.

*Update:* the change is no longer uncommitted. Commit `6f850fb` added `bno055_support.c` to the build on purpose, and `CLAUDE.md` notes that it is compiled in but never called. The risks above still apply.

**Recommendation**: revert the CMake change. Better still, move `bno055_support.c` into `docs/` or delete it, so it is not mistaken for part of the build. If the change was made to fix a link error, the missing symbol should be reported. It is not one this component defines or references.

---

## I4. Low: device-level notes

| Location | Note |
|----------|------|
| [bno055_device.cpp:25,39-42](../components/imu_bmo_055/src/bno055_device.cpp) | One global `s_active_device` is used for the thunks because the Bosch driver is a singleton (`p_bno055`). This is enforced at runtime only. A `static_assert`-style note in the header would help. If the constructor fails after `s_active_device = this` but before `m_valid`, it is cleared correctly. |
| [bno055_device.cpp:63-73](../components/imu_bmo_055/src/bno055_device.cpp) | `i2c_driver_install` returning `ESP_ERR_INVALID_STATE` (already installed by someone else) is accepted, but then `i2c_param_config` has just reconfigured *their* bus (pins and speed). Either own the bus or don't touch its config. |
| [bno055_device.cpp:3](../components/imu_bmo_055/src/bno055_device.cpp) | Uses the legacy `driver/i2c.h` API, which is deprecated in IDF 5.x and prints a warning. Plan to migrate to `driver/i2c_master.h`. |
| [bno055_device.cpp:432-436](../components/imu_bmo_055/src/bno055_device.cpp) | 256-byte stack buffer per write. Bosch writes are ≤ 22 bytes (SIC matrix 18). A 32-byte buffer plus an assert is enough and saves stack on the config task. |
| [bno055_device.cpp:439-460](../components/imu_bmo_055/src/bno055_device.cpp) | The recent "targeted retry" special-cases `PAGE_ID` writes with 3 attempts and a 500 ms timeout. That treats a symptom. The likely root cause is the 100 ms power-up delay at line 82: the BNO055 needs ~650 ms from POR to config mode (datasheet table 0-2). Wait for the chip-ID register to read `0xA0` (poll with a timeout) before the first write, then drop the special case. See [07](07-code-quality-and-simplification.md) Q4. |
| [bno055_device.cpp:175](../components/imu_bmo_055/src/bno055_device.cpp) | The sample timestamp is taken *before* the I²C reads. With every field enabled that is ~9 transactions, a few ms at 100 kHz, so the timestamp lags the data. Take it after the Euler/quaternion read, or in the middle. **Fixed:** `tryReadSample` stamps the wall-clock midpoint of the I²C burst. |
| [imu_bmo_055.cpp:299-302](../components/imu_bmo_055/src/imu_bmo_055.cpp) | `std::clamp(v, lo, hi)` is undefined behaviour if `min_stream_period_ms > max_stream_period_ms`, and both are runtime config. Validate them first. **Fixed:** the constructor rejects `min_sample_period_us == 0` or `min > max` before clamping. |
