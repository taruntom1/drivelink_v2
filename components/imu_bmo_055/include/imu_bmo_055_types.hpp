#pragma once

#include "imu_bmo_055_protocol.hpp"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace imu_bmo_055 {

#ifdef CONFIG_IMU_BMO_055_USE_MOCK_DATA
constexpr bool kDefaultUseMockData = true;
#else
constexpr bool kDefaultUseMockData = false;
#endif

#ifdef CONFIG_IMU_BMO_055_FALLBACK_TO_MOCK_ON_INIT_FAIL
constexpr bool kDefaultFallbackToMockOnInitFail = true;
#else
constexpr bool kDefaultFallbackToMockOnInitFail = false;
#endif

struct StreamTaskConfig {
    bool enable = true;
    /// Sampling timer period. Clamped to [min, max]_sample_period_us.
    uint32_t sample_period_us = CONFIG_IMU_BMO_055_DEFAULT_SAMPLE_PERIOD_US;
    /// Stream packet period; 0 = same as the sample period. Rounded up to a
    /// whole multiple of the sample period.
    uint32_t telemetry_period_us = CONFIG_IMU_BMO_055_DEFAULT_TELEMETRY_PERIOD_US;
    uint32_t stack_size = CONFIG_IMU_BMO_055_STREAM_TASK_STACK;
    UBaseType_t priority = CONFIG_IMU_BMO_055_STREAM_TASK_PRIO;
};

struct I2cConfig {
    int port_num = CONFIG_IMU_BMO_055_I2C_PORT_NUM;
    int sda_pin = CONFIG_IMU_BMO_055_I2C_SDA_PIN;
    int scl_pin = CONFIG_IMU_BMO_055_I2C_SCL_PIN;
    uint32_t clk_speed_hz = CONFIG_IMU_BMO_055_I2C_CLK_HZ;
    uint32_t timeout_ms = CONFIG_IMU_BMO_055_I2C_TIMEOUT_MS;
    uint8_t device_address = CONFIG_IMU_BMO_055_DEVICE_ADDRESS;
};

struct ManagerConfig {
    uint8_t configure_msg_type = kImuConfigureMsgType;
    uint8_t configure_response_msg_type = kImuConfigureResponseMsgType;
    uint8_t telemetry_stream_channel = kImuStreamChannel;
    bool use_mock_data = kDefaultUseMockData;
    bool fallback_to_mock_on_init_failure = kDefaultFallbackToMockOnInitFail;
    uint8_t default_operation_mode = CONFIG_IMU_BMO_055_DEFAULT_OPERATION_MODE;
    uint16_t default_stream_field_mask = CONFIG_IMU_BMO_055_DEFAULT_STREAM_FIELD_MASK;
    /// Clamp bounds for the sample period. max is also the telemetry
    /// period bound. The constructor fails if min > max or min == 0.
    uint32_t min_sample_period_us = CONFIG_IMU_BMO_055_MIN_SAMPLE_PERIOD_US;
    uint32_t max_sample_period_us = CONFIG_IMU_BMO_055_MAX_SAMPLE_PERIOD_US;
    StreamTaskConfig stream_task;
    uint32_t config_task_stack_size = CONFIG_IMU_BMO_055_CONFIG_TASK_STACK;
    UBaseType_t config_task_priority = CONFIG_IMU_BMO_055_CONFIG_TASK_PRIO;
    UBaseType_t config_queue_depth = CONFIG_IMU_BMO_055_CONFIG_QUEUE_DEPTH;
    I2cConfig i2c;
};

/// Stream health counters since startup. Each one is a running total.
struct StreamCounters {
    /// Timer periods that elapsed without a sample (the task woke late).
    uint32_t late = 0;
    /// Ticks skipped because the device was busy (config command in progress).
    uint32_t busy = 0;
    /// Ticks whose I2C read failed.
    uint32_t read_errors = 0;
    /// Samples that could not be encoded (payload overflow).
    uint32_t encode_errors = 0;
};

const char *resultCodeToString(ResultCode code);

} // namespace imu_bmo_055
