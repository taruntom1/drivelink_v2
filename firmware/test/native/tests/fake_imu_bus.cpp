#include "fake_imu_bus.hpp"
#include <algorithm>
namespace fake_imu_bus {
std::array<Device,2> devices;
core_os::Err open_error = 0;
void reset() {
    devices = {}; open_error = 0;
    for (auto &d : devices) { d.regs[0] = 0xA0; d.regs[0x3D] = 0; }
}
void put16(Device &d, uint8_t reg, int16_t value) {
    d.regs[reg] = static_cast<uint8_t>(value); d.regs[reg+1] = static_cast<uint16_t>(value) >> 8;
}
}
namespace mod_imu_bno055 {
namespace {
class Fake final : public Bus {
public:
    explicit Fake(fake_imu_bus::Device &device) : d(device) { d.open = true; }
    ~Fake() override { d.open = false; }
    core_os::Err read(uint8_t reg, std::span<uint8_t> out) override {
        const auto e = transfer(false, reg, out.size()); if (e) { return e; }
        std::copy_n(d.regs.begin()+reg, out.size(), out.begin()); return 0;
    }
    core_os::Err write(uint8_t reg, std::span<const uint8_t> in) override {
        const auto e = transfer(true, reg, in.size()); if (e) { return e; }
        if (reg == 0x3D) {
            if (in[0] > 12 || (d.regs[reg] != 0 && in[0] != 0)) { return -109; }
            d.ready_at_ms = d.now_ms + (in[0] == 0 ? 19 : 7);
            if (d.regs[0x39] != 1) { d.regs[0x39] = in[0] == 0 ? 0 : (in[0] >= 8 ? 5 : 6); }
        }
        std::copy(in.begin(), in.end(), d.regs.begin()+reg); return 0;
    }
    void delayMs(uint32_t ms) override { d.now_ms += ms; }
private:
    core_os::Err transfer(bool write, uint8_t reg, size_t length) {
        if (d.transfers < d.trace.size()) { d.trace[d.transfers] = {write,reg,length,d.now_ms}; }
        ++d.transfers;
        if (d.fail_transfer == d.transfers || (write ? d.fail_write_reg : d.fail_read_reg) == reg) { return d.error; }
        if (d.now_ms < d.ready_at_ms) { ++d.bad_timing; return -108; }
        if (reg+length > d.regs.size()) { return -110; }
        if (reg != 7 && d.regs[7] != 0) { return -112; }
        if ((reg == 0x55 || reg == 0x43 || (write && (reg == 0x3B || reg == 0x3E || reg == 0x40 || reg == 0x41 || reg == 0x42))) &&
            d.regs[0x3D] != 0) { return -111; }
        return 0;
    }
    fake_imu_bus::Device &d;
};
}
std::unique_ptr<Bus> openBus(const BusConfig &config, core_os::Err &err) {
    err = fake_imu_bus::open_error;
    if (err) { return nullptr; }
    if (!validBusConfig(config)) { err = -1; return nullptr; }
    auto &d = fake_imu_bus::devices[config.port];
    if (d.open) { err = -106; return nullptr; }
    return std::make_unique<Fake>(d);
}
}
