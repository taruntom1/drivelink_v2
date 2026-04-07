/**
 * @file   bno055_device.hpp
 * @brief  Internal BNO055 device wrapper declarations for imu_bmo_055.
 *
 * Component header documentation.
 *
 * @author Project Contributors
 * @date   2026-04-07
 */
#pragma once

extern "C" {
#include "bno055.h"
}

#include "imu_bmo_055_types.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace imu_bmo_055 {

struct ImuStreamSample {
    uint64_t timestamp_us = 0;

    bno055_euler_t euler{};
    bno055_quaternion_t quaternion{};
    bno055_linear_accel_t linear_accel{};
    bno055_gravity_t gravity{};
    bno055_accel_t raw_accel{};
    bno055_gyro_t raw_gyro{};
    bno055_mag_t raw_mag{};
    int8_t temperature = 0;
    CalibrationStatus calibration_status{};
};

class Bno055Device {
public:
    explicit Bno055Device(const I2cConfig &config);
    ~Bno055Device();

    Bno055Device(const Bno055Device &) = delete;
    Bno055Device &operator=(const Bno055Device &) = delete;
    Bno055Device(Bno055Device &&) = delete;
    Bno055Device &operator=(Bno055Device &&) = delete;

    bool isValid() const;

    esp_err_t setOperationMode(uint8_t operation_mode);
    esp_err_t getOperationMode(uint8_t &operation_mode) const;
    esp_err_t readSample(uint16_t field_mask, ImuStreamSample &sample) const;
    esp_err_t readCalibration(CalibrationBlob &out) const;
    esp_err_t writeCalibration(const CalibrationBlob &blob);

private:
    static s8 busWriteThunk(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt);
    static s8 busReadThunk(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt);
    static void delayThunk(u32 delay_ms);

    s8 busWrite(u8 reg_addr, const u8 *reg_data, u8 cnt) const;
    s8 busRead(u8 reg_addr, u8 *reg_data, u8 cnt) const;

    esp_err_t lock(TickType_t timeout) const;
    void unlock() const;

    static Bno055Device *s_active_device;

    struct bno055_t m_dev;
    I2cConfig m_config;
    mutable SemaphoreHandle_t m_mutex;
    bool m_valid;
    bool m_driver_installed;
};

} // namespace imu_bmo_055
