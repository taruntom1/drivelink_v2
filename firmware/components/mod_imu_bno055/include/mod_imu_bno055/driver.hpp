#pragma once
#include "mod_imu_bno055/bus.hpp"
#include "mod_imu_bno055/state.hpp"

namespace mod_imu_bno055 {
// Pure, instance-owned BNO055 register logic. No driver-global context.
class Driver {
public:
    explicit Driver(Bus &bus) : m_bus(bus) {}
    core_os::Err init();
    core_os::Err setMode(uint8_t mode);
    core_os::Err readSample(Sample &out);
    core_os::Err readCalibration(Calibration &out);
    core_os::Err applyCalibration(const Calibration &cal);
    uint8_t mode() const { return m_mode; }
private:
    core_os::Err writeByte(uint8_t reg, uint8_t value);
    core_os::Err calibration(Calibration &cal, bool write);
    core_os::Err waitMode(uint8_t expected);
    Bus &m_bus;
    uint8_t m_mode = 255;
};
} // namespace mod_imu_bno055
