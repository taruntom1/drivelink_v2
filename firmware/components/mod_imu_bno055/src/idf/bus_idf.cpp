#include "mod_imu_bno055/bus.hpp"
#include "core/core_config.hpp"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tick_util.hpp"
#include <array>
#include <algorithm>

namespace mod_imu_bno055 {
namespace {
// Protect setup/teardown only. Transfers belong to one instance/owning loop,
// or the config caller after that loop acknowledges its paused action.
struct Pool {
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    std::array<bool,2> claimed{};
};
Pool &pool() { static Pool value; return value; }

class IdfBus final : public Bus {
public:
    explicit IdfBus(const BusConfig &config) : m_config(config) {}
    core_os::Err init() {
        if (!validBusConfig(m_config)) { return ESP_ERR_INVALID_ARG; }
        const auto allowed = core::coreConfigFromKconfig().gpio_allowed_mask;
        if (!(allowed & core::gpio_policy::gpioBit(m_config.sda)) ||
            !(allowed & core::gpio_policy::gpioBit(m_config.scl))) { return ESP_ERR_INVALID_ARG; }
        auto &p = pool();
        if (!p.mutex) { return ESP_ERR_NO_MEM; }
        if (xSemaphoreTake(p.mutex, rtos_util::msToTicksCeil(100)) != pdTRUE) { return ESP_ERR_TIMEOUT; }
        if (p.claimed[m_config.port]) { xSemaphoreGive(p.mutex); return ESP_ERR_NOT_FOUND; }
        p.claimed[m_config.port] = true; m_claimed = true;
        xSemaphoreGive(p.mutex);
        i2c_master_bus_config_t bus{};
        bus.i2c_port = m_config.port;
        bus.sda_io_num = static_cast<gpio_num_t>(m_config.sda);
        bus.scl_io_num = static_cast<gpio_num_t>(m_config.scl);
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        auto err = i2c_new_master_bus(&bus, &m_bus);
        if (err) { return err; }
        i2c_device_config_t dev{};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = m_config.address;
        dev.scl_speed_hz = m_config.hz;
        return i2c_master_bus_add_device(m_bus, &dev, &m_device);
    }
    ~IdfBus() override {
        if (m_device) { (void)i2c_master_bus_rm_device(m_device); }
        if (m_bus) { (void)i2c_del_master_bus(m_bus); }
        if (m_claimed) {
            auto &p = pool();
            // Configure-time ownership release must complete before reuse.
            if (xSemaphoreTake(p.mutex, rtos_util::msToTicksCeil(100)) == pdTRUE) {
                p.claimed[m_config.port] = false; xSemaphoreGive(p.mutex);
            }
        }
    }
    core_os::Err read(uint8_t reg, std::span<uint8_t> out) override {
        if (out.empty() || out.size() > 54) { return ESP_ERR_INVALID_ARG; }
        return i2c_master_transmit_receive(m_device, &reg, 1, out.data(), out.size(), driverTimeoutMs());
    }
    core_os::Err write(uint8_t reg, std::span<const uint8_t> bytes) override {
        if (bytes.empty() || bytes.size() > 22) { return ESP_ERR_INVALID_ARG; }
        std::array<uint8_t,23> tx{}; tx[0] = reg;
        std::copy(bytes.begin(), bytes.end(), tx.begin()+1);
        return i2c_master_transmit(m_device, tx.data(), bytes.size()+1, driverTimeoutMs());
    }
    void delayMs(uint32_t ms) override { if (ms) { vTaskDelay(rtos_util::msToTicksCeil(ms)+1); } }
private:
    // IDF truncates ms to ticks and a wait may finish one tick early. Round
    // up and add one: requested 10..20 ms becomes a bounded 20..30 ms wait
    // at the board's 100 Hz tick rate. IDF may wait for FIFO progress more
    // than once, so this is a per-wait bound. A pathological bus or several
    // instances can still trip the unchanged loop-stall monitor.
    int driverTimeoutMs() const {
        return static_cast<int>((rtos_util::msToTicksCeil(m_config.timeout_ms)+1)*portTICK_PERIOD_MS);
    }
    BusConfig m_config;
    bool m_claimed = false;
    i2c_master_bus_handle_t m_bus = nullptr;
    i2c_master_dev_handle_t m_device = nullptr;
};
}
std::unique_ptr<Bus> openBus(const BusConfig &config, core_os::Err &err) {
    auto bus = std::make_unique<IdfBus>(config);
    err = bus->init();
    if (err) { return nullptr; }
    return bus;
}
} // namespace mod_imu_bno055
