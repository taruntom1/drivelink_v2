#pragma once
#include "core_os/types.hpp"
#include <memory>
#include <span>

namespace mod_imu_bno055 {
struct BusConfig {
    uint8_t port = 0, sda = 255, scl = 255, address = 0x28;
    uint32_t hz = 400000;
    uint16_t timeout_ms = 10;
};
// A 54-byte sample is about 5.1 ms at 100 kHz before stretching/scheduling.
// Restrict the supported clocks to sustain the factory 100 Hz usage.
constexpr bool validBusConfig(const BusConfig &c) {
    return c.port <= 1 && c.sda <= 48 && c.scl <= 48 && c.sda != c.scl &&
        c.address >= 0x28 && c.address <= 0x29 && c.hz >= 100000 && c.hz <= 400000 &&
        c.timeout_ms >= 10 && c.timeout_ms <= 20;
}
// Blocking interface; only a non-real-time module's boot, tick and paused
// action may use it. Driver waits use a bounded tick-rounded timeout; see
// the backend (10..20 ms requested becomes 20..30 ms per wait at 100 Hz).
class Bus {
public:
    virtual ~Bus() = default;
    virtual core_os::Err read(uint8_t reg, std::span<uint8_t> out) = 0;
    virtual core_os::Err write(uint8_t reg, std::span<const uint8_t> bytes) = 0;
    virtual void delayMs(uint32_t ms) = 0; // minimum settling time
};
std::unique_ptr<Bus> openBus(const BusConfig &config, core_os::Err &err);
} // namespace mod_imu_bno055
