#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motor_controller.hpp"
#include "sdkconfig.h"
#include <cstddef>
#include <cstdint>

namespace motor_manager {

#ifdef CONFIG_MOTOR_MANAGER_USE_MOCK_FEEDBACK
constexpr bool kDefaultUseMockFeedback = true;
#else
constexpr bool kDefaultUseMockFeedback = false;
#endif

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
    SetTelemetryMotor = 0x0A
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

// v2 added a seq byte after the version in configure requests and responses.
// It covers every motor_manager payload, including both streams.
constexpr uint8_t kProtocolVersion = 0x02;
// Sent as subcommand and motor_id in a configure response when the request
// header could not be parsed (too short or wrong version).
constexpr uint8_t kUnparsedHeaderByte = 0xFF;
constexpr uint8_t kManagerMsgConfigure = CONFIG_MOTOR_MANAGER_CONFIGURE_MSG_TYPE;
constexpr uint8_t kManagerMsgConfigureResp = CONFIG_MOTOR_MANAGER_CONFIGURE_RESPONSE_MSG_TYPE;
constexpr uint8_t kManagerSetpointStreamChannel = CONFIG_MOTOR_MANAGER_SETPOINT_STREAM_CHANNEL;
constexpr uint8_t kManagerTelemetryStreamChannel = CONFIG_MOTOR_MANAGER_TELEMETRY_STREAM_CHANNEL;

constexpr size_t kSetpointEntrySize = 6;  // motor_id(1) + kind(1) + float(4)

enum TelemetryFieldMask : uint8_t {
    kTelemetryFieldAngle = 1U << 0,
    kTelemetryFieldVelocity = 1U << 1,
    kTelemetryFieldAll = kTelemetryFieldAngle | kTelemetryFieldVelocity
};

constexpr size_t kTelemetryPacketHeaderSize = 10; // version(1) + entry_count(1) + timestamp_us(8)
constexpr size_t kTelemetryEntryBaseSize = 2;    // motor_id(1) + field_mask(1)

struct MotorConfigPayload {
    gpio_num_t pwm_gpio;
    gpio_num_t dir_gpio;
    gpio_num_t enc_a;
    gpio_num_t enc_b;
    float counts_per_rev;
    float gear_ratio;
    float output_min;
    float output_max;
    float vel_kp;
    float vel_ki;
    float vel_kd;
    float pos_kp;
    float pos_ki;
    float pos_kd;
    unsigned int sample_time_ms;
    float velocity_feedforward;
    float position_feedforward;
};

// Limits applied to the control and telemetry periods (constructor config and SetTiming).
constexpr uint32_t kControlPeriodMinUs = 250;
constexpr uint32_t kControlPeriodMaxUs = 100000;
constexpr uint32_t kTelemetryPeriodMaxUs = 1000000;

#if CONFIG_MOTOR_MANAGER_DEFAULT_CONTROL_TASK_CORE < 0
constexpr BaseType_t kDefaultControlTaskCore = tskNO_AFFINITY;
#else
constexpr BaseType_t kDefaultControlTaskCore = CONFIG_MOTOR_MANAGER_DEFAULT_CONTROL_TASK_CORE;
#endif

/// Internal control ("update") task. When enabled, a periodic esp_timer wakes
/// the task every control_period_us; each wake applies queued setpoints,
/// updates every motor with dt = elapsed ticks x period, and publishes
/// telemetry every ceil(telemetry_period_us / control_period_us) ticks.
/// Both periods can be changed at runtime with MotorManager::setTiming().
struct UpdateTaskConfig {
    bool enable = true;
    /// Control loop period, clamped to [kControlPeriodMinUs, kControlPeriodMaxUs].
    uint32_t control_period_us = CONFIG_MOTOR_MANAGER_DEFAULT_CONTROL_PERIOD_US;
    /// Telemetry period, clamped to [control period, kTelemetryPeriodMaxUs] and
    /// rounded up to a whole number of control periods.
    uint32_t telemetry_period_us = CONFIG_MOTOR_MANAGER_DEFAULT_TELEMETRY_PERIOD_US;
    uint32_t stack_size = CONFIG_MOTOR_MANAGER_DEFAULT_UPDATE_TASK_STACK;
    UBaseType_t priority = CONFIG_MOTOR_MANAGER_DEFAULT_UPDATE_TASK_PRIO;
    /// Core the control task is pinned to, or tskNO_AFFINITY.
    BaseType_t core_id = kDefaultControlTaskCore;
};

struct ManagerConfig {
    uint8_t configure_msg_type = kManagerMsgConfigure;
    uint8_t configure_response_msg_type = kManagerMsgConfigureResp;
    uint8_t setpoint_stream_channel = kManagerSetpointStreamChannel;
    uint8_t telemetry_stream_channel = kManagerTelemetryStreamChannel;
    bool use_mock_feedback = kDefaultUseMockFeedback;
    uint8_t max_motors = 8;
    UpdateTaskConfig update_task;
    uint32_t config_task_stack_size = CONFIG_MOTOR_MANAGER_DEFAULT_CONFIG_TASK_STACK;
    UBaseType_t config_task_priority = CONFIG_MOTOR_MANAGER_DEFAULT_CONFIG_TASK_PRIO;
};

const char *resultCodeToString(ResultCode code);
esp_err_t resultCodeToEspErr(ResultCode code);

} // namespace motor_manager
