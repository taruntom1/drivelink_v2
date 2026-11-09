// main.cpp - DriveLink v2 entry point.
//
// Startup order: diag, UartComm, the managers, then the diag Reporter.
// A manager that fails to start is reported (system.ManagerStartFailed),
// destroyed and left out; the others keep running, so the host can still
// connect and ask what went wrong. app_main never returns.

#include <optional>

#include "diag.hpp"
#include "diag_reporter.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu_bmo_055.hpp"
#include "motor_manager.hpp"
#include "time_sync.hpp"
#include "uart_comm.hpp"

static const char *const TAG = "main";

namespace {

[[noreturn]] void idleForever()
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

} // namespace

extern "C" void app_main(void)
{
    // First, so that anything below can report.
    diag::init();

    uart_comm::UartCommConfig uart_cfg{}; // Kconfig defaults
    uart_comm::UartComm uart(uart_cfg);
    if (!uart.isValid()) {
        // There is no link to report on. Stay alive so the console stays usable.
        ESP_LOGE(TAG, "UartComm failed to start (port=%d tx=%d rx=%d); nothing can be reported to the host",
                 uart_cfg.port_num, uart_cfg.tx_pin, uart_cfg.rx_pin);
        idleForever();
    }

    uint8_t started_mask = 0;

    motor_manager::ManagerConfig mgr_cfg{};
    mgr_cfg.max_motors = 4;
    mgr_cfg.configure_msg_type = motor_manager::kManagerMsgConfigure;                 // 0x21
    mgr_cfg.configure_response_msg_type = motor_manager::kManagerMsgConfigureResp;    // 0x61
    mgr_cfg.setpoint_stream_channel = motor_manager::kManagerSetpointStreamChannel;   // 0x80
    mgr_cfg.telemetry_stream_channel = motor_manager::kManagerTelemetryStreamChannel; // 0x81
    mgr_cfg.use_mock_feedback = true;
    mgr_cfg.update_task.enable = true; // timer-driven; periods from Kconfig, changeable via SetTiming

    std::optional<motor_manager::MotorManager> manager;
    manager.emplace(uart, mgr_cfg);
    if (manager->isValid()) {
        started_mask |= diag::kStartedMotorManager;
    } else {
        DIAG_ERROR(diag::codes::system::kManagerStartFailed, ESP_FAIL, diag::kManagerIdMotorManager, 0,
                   "Motor manager failed to start; continuing without it");
        manager.reset();
    }

    time_sync::TimeSyncConfig sync_cfg{};
    sync_cfg.sync_interval_ms = 5000;
    sync_cfg.response_timeout_ms = 250;
    sync_cfg.enable_round_trip_compensation = true;

    std::optional<time_sync::TimeSync> time_sync_obj;
    time_sync_obj.emplace(uart, sync_cfg);
    if (time_sync_obj->isValid()) {
        started_mask |= diag::kStartedTimeSync;
    } else {
        DIAG_ERROR(diag::codes::system::kManagerStartFailed, ESP_FAIL, diag::kManagerIdTimeSync, 0,
                   "Time sync failed to start; continuing without it");
        time_sync_obj.reset();
    }

    imu_bmo_055::ManagerConfig imu_cfg{};
    imu_cfg.use_mock_data = false;

    std::optional<imu_bmo_055::ImuBno055Manager> imu_manager;
    imu_manager.emplace(uart, imu_cfg);
    if (imu_manager->isValid()) {
        started_mask |= diag::kStartedImu;
    } else {
        DIAG_ERROR(diag::codes::system::kManagerStartFailed, ESP_FAIL, diag::kManagerIdImu, 0,
                   "IMU manager failed to start; continuing without it");
        imu_manager.reset();
    }

    if (time_sync_obj) {
        const esp_err_t ret = time_sync_obj->startPeriodicSync();
        if (ret != ESP_OK) {
            DIAG_ERROR(diag::codes::system::kPeriodicSyncStartFailed, ret, 0, 0,
                       "Periodic time sync not started: %s", esp_err_to_name(ret));
        }
    }

    // Last, so Boot carries the final started_mask. Records from above are
    // waiting in the diag queue and go out right after Boot.
    diag::ReporterConfig diag_cfg{};
    diag_cfg.started_mask = started_mask;
    diag::Reporter reporter(uart, diag_cfg);
    if (!reporter.isValid()) {
        ESP_LOGE(TAG, "diag reporter failed to start; errors are on the console only");
    }

    idleForever();
}
