#pragma once

// Host-test stand-in for the generated sdkconfig.h. It defines only the
// symbols that the pure sources under test read, with their Kconfig
// defaults. Keep the values in sync with the components' Kconfig files.

// pid
#define CONFIG_PID_DEFAULT_OUTPUT_MIN -1000
#define CONFIG_PID_DEFAULT_OUTPUT_MAX 1000
#define CONFIG_PID_DEFAULT_DERIVATIVE_CUTOFF_HZ 0

// imu_bmo_055 (imu_bmo_055_protocol.hpp)
#define CONFIG_IMU_BMO_055_CONFIGURE_MSG_TYPE 0x22
#define CONFIG_IMU_BMO_055_CONFIGURE_RESPONSE_MSG_TYPE 0x42
#define CONFIG_IMU_BMO_055_EVENT_MSG_TYPE 0xC2
#define CONFIG_IMU_BMO_055_STREAM_CHANNEL 0x82
