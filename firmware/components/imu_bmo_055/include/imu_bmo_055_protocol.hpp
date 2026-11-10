#pragma once

#include <cstddef>
#include <cstdint>
#include "sdkconfig.h"

namespace imu_bmo_055 {

enum class ConfigureSubcommand : uint8_t {
    SetStreamEnabled = 0x01,
    SetStreamTiming = 0x02,
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

// v3: SetStreamTiming (u32 sample and telemetry periods in us, applied
// values echoed) replaces SetStreamPeriodMs; configure bodies must have their
// exact length; the stream timestamp is wall-clock time.
// v4: a detail byte follows result in configure responses.
// v5: no payload change; bumped with the link framing (u16 LEN + HCRC, overhaul F1b).
// The version covers every IMU payload, including the stream.
constexpr uint8_t kProtocolVersion = 0x05;
// Sent as the subcommand in a configure response when the request header
// could not be parsed (too short or wrong version).
constexpr uint8_t kUnparsedHeaderByte = 0xFF;

constexpr uint8_t kImuConfigureMsgType = CONFIG_IMU_BMO_055_CONFIGURE_MSG_TYPE;
constexpr uint8_t kImuConfigureResponseMsgType = CONFIG_IMU_BMO_055_CONFIGURE_RESPONSE_MSG_TYPE;
constexpr uint8_t kImuStreamChannel = CONFIG_IMU_BMO_055_STREAM_CHANNEL;

enum class ResultCode : uint8_t {
    Ok = 0x00,
    InvalidArgument = 0x01,
    InvalidState = 0x02,
    NotSupported = 0x03,
    Busy = 0x04,
    InternalError = 0x05
};

/// Why a configure command failed: the byte after `result` in every
/// configure response (v4). None when the result is Ok. The numbers are part
/// of the wire format: never renumber.
enum class ResultDetail : uint8_t {
    None = 0x00,
    BadVersion = 0x01,        ///< wrong protocol version (header not parsed)
    BadLength = 0x02,         ///< header too short, or body not its exact size
    UnknownSubcommand = 0x03, ///< with NotSupported
    BadEnum = 0x04,           ///< enabled flag or has_sic not 0/1, or operation mode out of range
    BadFieldMask = 0x05,      ///< stream field bits outside kStreamFieldAll
    NotReady = 0x06,          ///< the manager failed to start (InvalidState)
    QueueFull = 0x07,         ///< config queue full, command dropped (Busy)
    LockTimeout = 0x08,       ///< manager mutex busy for the whole timeout (Busy)
    DeviceBusy = 0x09,        ///< BNO055 lock busy for the whole timeout (Busy)
    DeviceError = 0x0A,       ///< BNO055 / I2C operation failed (InternalError)
    TimerFailed = 0x0B,       ///< the sampling timer could not be (re)started (InternalError)
    NoCalibration = 0x0C,     ///< ApplyCalibration with nothing read or written yet (InvalidState)
    EncodeFailed = 0x0D,      ///< the response body did not fit (InternalError)
};

/// A result code and why. Default-constructed it is Ok / None.
struct Result {
    ResultCode code = ResultCode::Ok;
    ResultDetail detail = ResultDetail::None;

    constexpr Result() = default;
    constexpr Result(ResultCode c, ResultDetail d) : code(c), detail(d) {}
    constexpr bool ok() const { return code == ResultCode::Ok; }
};

struct CalibrationStatus {
    uint8_t sys = 0;
    uint8_t gyro = 0;
    uint8_t accel = 0;
    uint8_t mag = 0;
};

struct CalibrationBlob {
    int16_t accel_offset_x = 0;
    int16_t accel_offset_y = 0;
    int16_t accel_offset_z = 0;
    int16_t accel_radius = 0;

    int16_t gyro_offset_x = 0;
    int16_t gyro_offset_y = 0;
    int16_t gyro_offset_z = 0;

    int16_t mag_offset_x = 0;
    int16_t mag_offset_y = 0;
    int16_t mag_offset_z = 0;
    int16_t mag_radius = 0;

    int16_t sic[9] = {0};
    bool has_sic = false;
};

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
constexpr size_t kConfigureRequestHeaderSize = 3U;  // version + seq + subcommand
constexpr size_t kConfigureResponseHeaderSize = 5U; // version + seq + subcommand + result + detail

// Exact configure body sizes (after the request header).
constexpr size_t kSetStreamEnabledBodySize = 1U;
constexpr size_t kSetStreamTimingBodySize = 8U;   // u32 sample_period_us + u32 telemetry_period_us
constexpr size_t kSetStreamFieldMaskBodySize = 2U;
constexpr size_t kSetOperationModeBodySize = 1U;
constexpr size_t kCalibrationBlobSize = 41U;
constexpr size_t kSetStreamTimingResponseBodySize = 8U; // applied sample + telemetry period

} // namespace imu_bmo_055
