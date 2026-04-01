#pragma once

#include "imu_bmo_055_protocol.hpp"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace imu_bmo_055 {

enum class ResultCode : uint8_t {
    Ok = 0x00,
    InvalidArgument = 0x01,
    InvalidState = 0x02,
    NotSupported = 0x03,
    Busy = 0x04,
    InternalError = 0x05
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

struct StreamTaskConfig {
    bool enable = true;
    uint16_t period_ms = CONFIG_IMU_BMO_055_DEFAULT_STREAM_PERIOD_MS;
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
    uint8_t event_msg_type = kImuEventMsgType;
    uint8_t telemetry_stream_channel = kImuStreamChannel;
    uint8_t default_operation_mode = CONFIG_IMU_BMO_055_DEFAULT_OPERATION_MODE;
    uint16_t default_stream_field_mask = CONFIG_IMU_BMO_055_DEFAULT_STREAM_FIELD_MASK;
    uint16_t min_stream_period_ms = CONFIG_IMU_BMO_055_MIN_STREAM_PERIOD_MS;
    uint16_t max_stream_period_ms = CONFIG_IMU_BMO_055_MAX_STREAM_PERIOD_MS;
    StreamTaskConfig stream_task;
    uint32_t config_task_stack_size = CONFIG_IMU_BMO_055_CONFIG_TASK_STACK;
    UBaseType_t config_task_priority = CONFIG_IMU_BMO_055_CONFIG_TASK_PRIO;
    UBaseType_t config_queue_depth = CONFIG_IMU_BMO_055_CONFIG_QUEUE_DEPTH;
    I2cConfig i2c;
};

const char *resultCodeToString(ResultCode code);

} // namespace imu_bmo_055
