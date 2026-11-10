#pragma once

extern "C" {
#include "bno055.h"
}

#include "imu_bmo_055_types.hpp"
#include "imu_protocol.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace imu_bmo_055 {

class Bno055Device {
public:
    explicit Bno055Device(const I2cConfig &config);
    ~Bno055Device();

    Bno055Device(const Bno055Device &) = delete;
    Bno055Device &operator=(const Bno055Device &) = delete;
    Bno055Device(Bno055Device &&) = delete;
    Bno055Device &operator=(Bno055Device &&) = delete;

    bool isValid() const;
    /// Why construction failed: ESP_OK when valid. ESP_ERR_NO_MEM (mutex),
    /// ESP_ERR_INVALID_STATE (a second instance), the I2C driver error,
    /// ESP_FAIL (bno055_init: no answer on the bus) or
    /// ESP_ERR_INVALID_RESPONSE (wrong chip id).
    esp_err_t initError() const;

    esp_err_t setOperationMode(uint8_t operation_mode);
    esp_err_t getOperationMode(uint8_t &operation_mode) const;
    /// Read the fields in `field_mask` for the stream tick. Never waits for
    /// the device lock. timestamp_us is wall-clock time at the midpoint of
    /// the I2C reads.
    /// @return ESP_OK; ESP_ERR_TIMEOUT if the device is busy (lock held by a
    ///         config operation); ESP_FAIL if an I2C read failed.
    esp_err_t tryReadSample(uint16_t field_mask, ImuStreamSample &sample) const;
    esp_err_t readCalibration(CalibrationBlob &out) const;
    esp_err_t writeCalibration(const CalibrationBlob &blob);

private:
    static s8 busWriteThunk(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt);
    static s8 busReadThunk(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt);
    static void delayThunk(u32 delay_ms);

    s8 busWrite(u8 reg_addr, const u8 *reg_data, u8 cnt) const;
    s8 busRead(u8 reg_addr, u8 *reg_data, u8 cnt) const;
    /// Rate limit for bus error logs: true at most once per second; the
    /// calls in between are counted in m_suppressed_bus_logs.
    bool shouldLogBusError() const;

    esp_err_t lock(TickType_t timeout) const;
    void unlock() const;

    static Bno055Device *s_active_device;

    struct bno055_t m_dev;
    I2cConfig m_config;
    mutable SemaphoreHandle_t m_mutex;
    bool m_valid;
    esp_err_t m_init_error;
    bool m_driver_installed;
    // Bus error log rate limit. Only touched under m_mutex (or during
    // construction, before anything else can use the device).
    mutable int64_t m_last_bus_log_us;
    mutable uint32_t m_suppressed_bus_logs;
};

} // namespace imu_bmo_055
