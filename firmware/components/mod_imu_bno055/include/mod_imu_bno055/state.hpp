#pragma once

#include "core/instance.hpp"
#include <array>

namespace mod_imu_bno055 {

// Shared item IDs in imu.bno055.yaml and imu.mock.yaml. These remain stable
// within each type; the host discovers them rather than compiling this table.
inline constexpr uint16_t kCalibrationValid = 64, kCalibrationHasSic = 65;
inline constexpr uint16_t kCalibrationFirst = 66, kSignalFirst = 256;
inline constexpr uint16_t kCalibrationRead = 770, kCalibrationWrite = 771;
inline constexpr uint16_t kCalibrationApply = 772, kSetMode = 773;
inline constexpr size_t kCalibrationBlobBytes = 42;

struct Calibration {
    bool valid = false, has_sic = false;
    // accel xyz/radius, gyro xyz, mag xyz/radius, SIC matrix row-major.
    std::array<int32_t,20> values{};
};

bool decodeCalibration(std::span<const uint8_t> blob, Calibration &out);
bool encodeCalibration(const Calibration &cal, std::span<uint8_t> out);
void calibrationUpdates(const Calibration &cal, core::ActionOut &out);
bool bindCalibration(core::InstanceContext &ctx, Calibration &cal);

// A successful I2C burst is decoded off to the side before replacing this
// entire sample. Availability bits: Euler, quaternion, linear acceleration,
// gravity, accelerometer, gyroscope, magnetometer, temperature, calibration.
struct Sample {
    float yaw = 0, roll = 0, pitch = 0;
    std::array<float,4> quaternion{1,0,0,0};
    std::array<float,3> linear_accel{}, gravity{}, accel{}, gyro{}, mag{};
    int32_t temperature = 0;
    std::array<uint8_t,4> calibration{}; // sys, gyro, accel, mag
};

struct State {
    Calibration staged;
    Sample sample;
    bool valid = false;
    uint32_t age_us = UINT32_MAX, count = 0;
    uint16_t available = 0;
    uint8_t actual_mode = 255, startup_mode = 12;
    int64_t last_sample_us = -1;
    bool bind(core::InstanceContext &ctx);
    void age(int64_t now_us);
    void publish(const Sample &value, int64_t now_us);
};

uint16_t modeAvailability(uint8_t mode);
core::Status badCalibration();

} // namespace mod_imu_bno055
