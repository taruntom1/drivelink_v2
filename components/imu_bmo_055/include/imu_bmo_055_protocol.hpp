/**
 * @file   imu_bmo_055_protocol.hpp
 * @brief  Public protocol declarations for imu_bmo_055.
 *
 * Component header documentation.
 *
 * @author Project Contributors
 * @date   2026-04-07
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include "sdkconfig.h"

namespace imu_bmo_055 {

enum class ConfigureSubcommand : uint8_t {
    SetStreamEnabled = 0x01,
    SetStreamPeriodMs = 0x02,
    SetStreamFieldMask = 0x03,
    SetOperationMode = 0x04,
    ReadCalibration = 0x05,
    WriteCalibration = 0x06,
    ApplyCalibration = 0x07
};

enum class StreamField : uint16_t {
    Euler = 1U << 0,
    Quaternion = 1U << 1,
    LinearAccel = 1U << 2,
    Gravity = 1U << 3,
    RawAccel = 1U << 4,
    RawGyro = 1U << 5,
    RawMag = 1U << 6,
    Temperature = 1U << 7,
    CalibrationStatus = 1U << 8
};

constexpr uint8_t kProtocolVersion = 0x01;

constexpr uint8_t kImuConfigureMsgType = CONFIG_IMU_BMO_055_CONFIGURE_MSG_TYPE;
constexpr uint8_t kImuConfigureResponseMsgType = CONFIG_IMU_BMO_055_CONFIGURE_RESPONSE_MSG_TYPE;
constexpr uint8_t kImuEventMsgType = CONFIG_IMU_BMO_055_EVENT_MSG_TYPE;
constexpr uint8_t kImuStreamChannel = CONFIG_IMU_BMO_055_STREAM_CHANNEL;

constexpr uint16_t kStreamFieldAll =
    static_cast<uint16_t>(StreamField::Euler) |
    static_cast<uint16_t>(StreamField::Quaternion) |
    static_cast<uint16_t>(StreamField::LinearAccel) |
    static_cast<uint16_t>(StreamField::Gravity) |
    static_cast<uint16_t>(StreamField::RawAccel) |
    static_cast<uint16_t>(StreamField::RawGyro) |
    static_cast<uint16_t>(StreamField::RawMag) |
    static_cast<uint16_t>(StreamField::Temperature) |
    static_cast<uint16_t>(StreamField::CalibrationStatus);

constexpr size_t kStreamHeaderSize = 11U; // version(1) + field_mask(2) + timestamp_us(8)
constexpr size_t kConfigureResponseHeaderSize = 3U; // version + subcommand + result

} // namespace imu_bmo_055
