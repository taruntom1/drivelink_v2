#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motor_controller.hpp"
#include "motor_manager_protocol.hpp"
#include "sdkconfig.h"
#include <cstddef>
#include <cstdint>

namespace motor_manager {

#ifdef CONFIG_MOTOR_MANAGER_USE_MOCK_FEEDBACK
constexpr bool kDefaultUseMockFeedback = true;
#else
constexpr bool kDefaultUseMockFeedback = false;
#endif

constexpr uint8_t kManagerMsgConfigure = CONFIG_MOTOR_MANAGER_CONFIGURE_MSG_TYPE;
constexpr uint8_t kManagerMsgConfigureResp = CONFIG_MOTOR_MANAGER_CONFIGURE_RESPONSE_MSG_TYPE;
constexpr uint8_t kManagerSetpointStreamChannel = CONFIG_MOTOR_MANAGER_SETPOINT_STREAM_CHANNEL;
constexpr uint8_t kManagerTelemetryStreamChannel = CONFIG_MOTOR_MANAGER_TELEMETRY_STREAM_CHANNEL;

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
