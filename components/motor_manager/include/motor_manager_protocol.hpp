#pragma once

/// motor_manager wire format: enums, field values and sizes shared by the
/// configure command, the setpoint stream and the telemetry stream. Pure C++
/// with no ESP-IDF dependency. The MSG_TYPEs and stream channels come from
/// Kconfig and are in motor_manager_types.hpp. README.md is the spec.

#include <cstddef>
#include <cstdint>

namespace motor_manager {

enum class ResultCode : uint8_t {
    Ok = 0x00,
    InvalidArgument = 0x01,
    NotFound = 0x02,
    AlreadyExists = 0x03,
    CapacityReached = 0x04,
    InvalidState = 0x05,
    NotSupported = 0x06,
    InternalError = 0x07
};

enum class ConfigureSubcommand : uint8_t {
    CreateMotor = 0x01,
    DeleteMotor = 0x02,
    ConfigureMotor = 0x03,
    SetMode = 0x04,
    SetPidParams = 0x05,
    SetSetpoint = 0x06,
    StopMotor = 0x07,
    ResetMotor = 0x08,
    SetTelemetryGlobal = 0x09,
    SetTelemetryMotor = 0x0A,
    SetTiming = 0x0B
};

/// Control mode as sent in SetMode.
enum class ControlModeCode : uint8_t {
    OpenLoop = 0x00,
    VelocityControl = 0x01,
    PositionControl = 0x02
};

enum class PidKind : uint8_t {
    Velocity = 0x00,
    Position = 0x01
};

enum class SetpointKind : uint8_t {
    OpenLoopDuty = 0x00,
    VelocityRadPerSec = 0x01,
    PositionRad = 0x02
};

constexpr bool isValidPidKind(PidKind kind)
{
    return kind == PidKind::Velocity || kind == PidKind::Position;
}

constexpr bool isValidSetpointKind(SetpointKind kind)
{
    return static_cast<uint8_t>(kind) <= static_cast<uint8_t>(SetpointKind::PositionRad);
}

// v2 added a seq byte after the version in configure requests and responses.
// v3 removed sample_time_ms and the reserved floats, added filter cutoffs,
// the SetTiming subcommand, response bodies and the telemetry output field.
// The version covers every motor_manager payload, including both streams.
constexpr uint8_t kProtocolVersion = 0x03;
// Sent as subcommand and motor_id in a configure response when the request
// header could not be parsed (too short or wrong version).
constexpr uint8_t kUnparsedHeaderByte = 0xFF;

constexpr size_t kConfigureHeaderSize = 4;         // version + seq + subcommand + motor_id
constexpr size_t kConfigureResponseHeaderSize = 5; // version + seq + subcommand + motor_id + result

// Exact configure body sizes (bytes after the configure header).
constexpr size_t kMotorConfigGpioCount = 4;
constexpr size_t kMotorConfigFloatCount = 15;
constexpr size_t kMotorConfigBodySize = kMotorConfigGpioCount + kMotorConfigFloatCount * 4; // 64
constexpr size_t kSetModeBodySize = 1;              // mode
constexpr size_t kSetPidParamsBodySize = 1 + 4 * 4; // kind + kp ki kd d_cutoff
constexpr size_t kSetSetpointBodySize = 1 + 4;      // kind + value
constexpr size_t kSetTelemetryGlobalBodySize = 2;   // enabled + field_mask
constexpr size_t kSetTelemetryMotorBodySize = 3;    // enabled + field_mask + apply
constexpr size_t kSetTimingBodySize = 2 * 4;        // control_us + telemetry_us
constexpr size_t kSetTimingResponseBodySize = 2 * 4; // applied control_us + telemetry_us

constexpr size_t kSetpointStreamHeaderSize = 2; // version + entry_count
constexpr size_t kSetpointEntrySize = 6;        // motor_id(1) + kind(1) + float(4)

enum TelemetryFieldMask : uint8_t {
    kTelemetryFieldAngle = 1U << 0,
    kTelemetryFieldVelocity = 1U << 1,
    kTelemetryFieldOutput = 1U << 2, ///< applied motor duty in [-1, 1]
    kTelemetryFieldAll = kTelemetryFieldAngle | kTelemetryFieldVelocity | kTelemetryFieldOutput
};

constexpr size_t kTelemetryPacketHeaderSize = 10; // version(1) + entry_count(1) + timestamp_us(8)
constexpr size_t kTelemetryEntryBaseSize = 2;     // motor_id(1) + field_mask(1)

/// Body of CreateMotor / ConfigureMotor, in wire order.
struct MotorConfigPayload {
    uint8_t pwm_gpio;
    uint8_t dir_gpio;
    uint8_t enc_a;
    uint8_t enc_b;
    float counts_per_rev;
    float gear_ratio;
    float output_min;
    float output_max;
    float vel_kp;
    float vel_ki;
    float vel_kd;
    float vel_d_cutoff_hz;      ///< velocity PID derivative filter cutoff (0 = off)
    float pos_kp;
    float pos_ki;
    float pos_kd;
    float pos_d_cutoff_hz;      ///< position PID derivative filter cutoff (0 = off)
    float velocity_feedforward;
    float position_feedforward;
    float vel_filter_cutoff_hz; ///< velocity estimate low-pass cutoff (0 = off)
};

} // namespace motor_manager
