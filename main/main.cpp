// main.cpp - Example entry point for ESP-IDF project using uart_comm and motor_manager

#include "uart_comm.hpp"
#include "motor_manager.hpp"
#include "time_sync.hpp"
#include "imu_bmo_055.hpp"

extern "C" void app_main(void)
{
    // Bring up UART communication
    uart_comm::UartCommConfig uart_cfg{}; // Uses Kconfig defaults
    uart_comm::UartComm uart(uart_cfg);
    if (!uart.isValid())
    {
        // UART initialization failed
        return;
    }

    // Configure motor manager
    motor_manager::ManagerConfig mgr_cfg{};
    mgr_cfg.max_motors = 4;
    mgr_cfg.configure_msg_type = motor_manager::kManagerMsgConfigure;                 // 0x21
    mgr_cfg.configure_response_msg_type = motor_manager::kManagerMsgConfigureResp;    // 0x61
    mgr_cfg.setpoint_stream_channel = motor_manager::kManagerSetpointStreamChannel;   // 0x80
    mgr_cfg.telemetry_stream_channel = motor_manager::kManagerTelemetryStreamChannel; // 0x81
    mgr_cfg.use_mock_feedback = true;
    mgr_cfg.update_task.enable = true;
    mgr_cfg.update_task.period_ms = 10;

    motor_manager::MotorManager manager(uart, mgr_cfg);
    if (!manager.isValid())
    {
        // Motor manager initialization failed
        return;
    }

    time_sync::TimeSyncConfig sync_cfg{};
    sync_cfg.sync_interval_ms = 5000;
    sync_cfg.response_timeout_ms = 250;
    sync_cfg.enable_round_trip_compensation = true;

    time_sync::TimeSync time_sync_obj(uart, sync_cfg);
    if (!time_sync_obj.isValid())
    {
        // Time sync initialization failed
        return;
    }

    imu_bmo_055::ManagerConfig imu_cfg{};
    imu_cfg.use_mock_data = true;
    imu_bmo_055::ImuBno055Manager imu_manager(uart, imu_cfg);
    if (!imu_manager.isValid())
    {
        // IMU manager initialization failed
        return;
    }

    if (time_sync_obj.startPeriodicSync() != ESP_OK)
    {
        // Time sync could not be started
        return;
    }


    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Main task can perform other work or sleep
    }
}
