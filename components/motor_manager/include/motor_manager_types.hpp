/**
 * @file   motor_manager_types.hpp
 * @brief  Public/internal declarations for motor_manager_types.
 *
 * Auto-generated documentation header for component API/internal declarations.
 *
 * @author Project Contributors
 * @date   2026-04-07
 */
#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
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

constexpr uint8_t kProtocolVersion = 0x01;
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

struct UpdateTaskConfig {
    bool enable = true;
    unsigned int period_ms = 10;
    uint32_t stack_size = CONFIG_MOTOR_MANAGER_DEFAULT_UPDATE_TASK_STACK;
    UBaseType_t priority = CONFIG_MOTOR_MANAGER_DEFAULT_UPDATE_TASK_PRIO;
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
