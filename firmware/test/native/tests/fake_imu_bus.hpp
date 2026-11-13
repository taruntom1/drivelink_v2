#pragma once
#include "mod_imu_bno055/bus.hpp"
#include <array>
namespace fake_imu_bus {
struct Transfer { bool write; uint8_t reg; size_t length; uint64_t at_ms; };
struct Device {
    bool open = false;
    std::array<uint8_t,128> regs{};
    std::array<Transfer,512> trace{};
    size_t transfers = 0;
    int fail_read_reg = -1, fail_write_reg = -1;
    size_t fail_transfer = 0;
    core_os::Err error = -107; // a bus timeout, distinguishable from validation
    uint64_t now_ms = 0, ready_at_ms = 0;
    unsigned bad_timing = 0;
};
extern std::array<Device,2> devices;
extern core_os::Err open_error;
void reset();
void put16(Device &d, uint8_t reg, int16_t value);
}
