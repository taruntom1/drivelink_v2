// main.cpp - DriveLink v2 entry point.
//
// Startup order: diag, UartComm, the managers, NVS, the core (init, boot from
// NVS, start), then the diag Reporter. A manager that fails to start is
// reported (system.ManagerStartFailed), destroyed and left out; the others
// keep running, so the host can still connect and ask what went wrong. The
// core is the same: if it fails it is reported (core.InitFailed) and left
// out. app_main never returns.

#include <array>
#include <memory>
#include <optional>
#include <span>

#include "core/core.hpp"
#include "core/core_link.hpp"
#include "mod_mock/modules.hpp"
#include "core/core_config.hpp"
#include "core/module_type.hpp"
#include "core/registry.hpp"
#include "core_os/kv_store.hpp"
#include "diag.hpp"
#include "diag_reporter.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/types.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu_bmo_055.hpp"
#include "motor_manager.hpp"
#include "time_sync.hpp"
#include "uart_comm.hpp"

static const char *const TAG = "main";

namespace {

#if CONFIG_DRIVELINK_STACK_LOG_PERIOD_S > 0 && CONFIG_DIAG_CONSOLE_LOG
#define DRIVELINK_LOG_STACKS 1

// The firmware's tasks, by the names they are created with. A name with no
// task (a manager that did not start) is skipped.
// esp_timer runs the timer callbacks (the control and IMU sampling timers).
constexpr std::array<const char *, 13> kLoggedTasks = {
    "main",       "uart_rx",    "uart_tx",        "motor_mgr_upd", "motor_mgr_cfg", "imu_cfg",
    "imu_stream", "time_sync_task", "diag",       "esp_timer",     "core_ctrl",     "core_sens", "core_cfg",
};

/// Log each task's stack high-water mark: the least free stack, in bytes,
/// since the task started. Used to size the task stacks.
/// uxTaskGetStackHighWaterMark() scans the stack without the kernel lock, and
/// xTaskGetHandle() holds it only to walk the task lists, so this doesn't
/// stall the control loop on the other core. (uxTaskGetSystemState() would
/// scan every stack inside the kernel's critical section.)
void logStackHighWater()
{
    for (const char *name : kLoggedTasks) {
        TaskHandle_t handle = xTaskGetHandle(name);
        if (handle != nullptr) {
            ESP_LOGI(TAG, "Stack high-water: %-15s %5u bytes free", name,
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(handle)));
        }
    }
}
#endif

/// The module types the core can instantiate. F3b adds the first ones. Each
/// desc must be its generated kTypeTable entry (checked in startCore()).
const auto &kModuleTypes=mod_mock::kTypes;

/// `core.NvsInitFailed` arg0 values. Part of the diag contract
/// (protocol/defs/diag_codes.yaml): never renumber.
constexpr uint32_t kNvsInitErased = 1;
constexpr uint32_t kNvsInitFailed = 2;

/// The core's NVS store. Static: it lives as long as the core, which is
/// never destroyed.
core_os::KvStore &kvStore()
{
    static core_os::KvStore store;
    return store;
}

/// Initialise NVS. A full partition or one from a newer NVS version is
/// erased and initialised again; both that and a failure are reported. The
/// core boots either way (with the defaults when nothing could be read).
void initNvs()
{
    const core_os::NvsInitResult r = core_os::nvsInit();
    if (r.erased) {
        DIAG_ERROR(diag::codes::core::kNvsInitFailed, r.first_err, kNvsInitErased, 0,
                   "NVS was erased and re-initialised (%s); the stored configuration is lost",
                   esp_err_to_name(r.first_err));
    }
    if (r.err != ESP_OK) {
        DIAG_ERROR(diag::codes::core::kNvsInitFailed, r.err, kNvsInitFailed, 0,
                   "NVS init failed: %s; nothing will be loaded or saved", esp_err_to_name(r.err));
    }
}

#if CONFIG_CORE_BOOT_SELFTEST
/// BENCH CHECK ONLY (CONFIG_CORE_BOOT_SELFTEST, default off; never commit it
/// on). Proves SAVE and the reload on hardware before F3b puts them on the
/// wire: each boot flips control.rate_hz between 1000 and 500 and SAVEs, so
/// the boot after a 1000 → 500 SAVE logs "reloaded 500 from NVS".
void bootSelftest(core::Core &c)
{
    namespace sys = drivelink::protocol::defs::system;
    const core::Ref rate{drivelink::protocol::defs::kSystemTypeId, 0, sys::kControlRateHz};
    uint32_t value = 0;
    if (!c.params().get({&rate, 1}, {&value, 1}).ok()) {
        ESP_LOGE(TAG, "selftest: cannot read control.rate_hz");
        return;
    }
    uint32_t next = 500;
    if (value == 500U) {
        ESP_LOGI(TAG, "selftest: reloaded 500 from NVS (control period %u us); restoring 1000",
                 static_cast<unsigned>(c.loop(core::kControlLoop).periodUs()));
        next = 1000;
    } else {
        ESP_LOGI(TAG, "selftest: control.rate_hz is %u; setting 500", static_cast<unsigned>(value));
    }
    const core::SetEntry set{rate, next};
    uint32_t applied = 0;
    core::Status st = c.params().set({&set, 1}, {&applied, 1});
    if (st.ok()) {
        st = c.save();
    }
    ESP_LOGI(TAG, "selftest: control.rate_hz %u and SAVE: result %u, detail %u; control period %u us",
             static_cast<unsigned>(applied), static_cast<unsigned>(st.result), static_cast<unsigned>(st.detail),
             static_cast<unsigned>(c.loop(core::kControlLoop).periodUs()));
}
#endif

/// Build, boot and start the core: the registry, the layout and params from
/// NVS (or the defaults), the instances (none until F3b brings module types)
/// and both loops. The Core lives on the heap: app_main's stack is small. On
/// failure it reports core.InitFailed and returns nullptr; the rest of the
/// firmware keeps running.
std::unique_ptr<core::Core> startCore()
{
    const std::span<const core::ModuleType *const> types(kModuleTypes);
    for (size_t i = 0; i < types.size(); ++i) {
        if (!core::descIsInTable(*types[i], drivelink::protocol::defs::kTypeTable)) {
            DIAG_ERROR(diag::codes::core::kInitFailed, ESP_ERR_INVALID_ARG, core::kInitStageRegistry, i,
                       "Core: module type %u (list index %u) is not its generated table entry",
                       static_cast<unsigned>(types[i]->desc.type_id), static_cast<unsigned>(i));
            return nullptr;
        }
    }

    auto c = std::make_unique<core::Core>(core::coreConfigFromKconfig());
    core::Status st = c->init(types);
    if (!st.ok()) {
        DIAG_ERROR(diag::codes::core::kInitFailed, ESP_ERR_INVALID_ARG, core::kInitStageRegistry, st.index,
                   "Core: module registry rejected list index %u (detail %u)", static_cast<unsigned>(st.index),
                   static_cast<unsigned>(st.detail));
        return nullptr;
    }
    st = c->boot(kvStore());
    if (!st.ok()) {
        DIAG_ERROR(diag::codes::core::kInitFailed, ESP_ERR_INVALID_STATE, core::kInitStageBoot, 0,
                   "Core: boot refused (result %u, detail %u)", static_cast<unsigned>(st.result),
                   static_cast<unsigned>(st.detail));
        return nullptr;
    }
    st = c->start();
    if (!st.ok()) {
        const esp_err_t err = static_cast<esp_err_t>(c->startError());
        DIAG_ERROR(diag::codes::core::kInitFailed, err, c->startStage(), 0,
                   "Core: loop task failed to start (stage %u): %s", static_cast<unsigned>(c->startStage()),
                   esp_err_to_name(err));
        return nullptr;
    }
    ESP_LOGI(TAG,
             "Core started: %u instances (%u failed, %u configured); safe mode %s; config %s; loops control %u us, "
             "sensor %u us",
             static_cast<unsigned>(c->instanceCount()), static_cast<unsigned>(c->instancesFailed()),
             static_cast<unsigned>(c->configuredCount()), c->safeMode() ? "ON" : "off",
             c->configLoadedFromNvs() ? "loaded from NVS" : "defaults",
             static_cast<unsigned>(c->loop(core::kControlLoop).periodUs()),
             static_cast<unsigned>(c->loop(core::kSensorLoop).periodUs()));
#if CONFIG_CORE_BOOT_SELFTEST
    bootSelftest(*c);
#endif
    return c;
}

[[noreturn]] void idleForever()
{
#if DRIVELINK_LOG_STACKS
    constexpr TickType_t kPeriod = pdMS_TO_TICKS(CONFIG_DRIVELINK_STACK_LOG_PERIOD_S * 1000);
    while (true) {
        vTaskDelay(kPeriod);
        logStackHighWater();
    }
#else
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
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
    mgr_cfg.setpoint_stream_channel = motor_manager::kManagerSetpointStreamChannel;   // 0xBD
    mgr_cfg.telemetry_stream_channel = motor_manager::kManagerTelemetryStreamChannel; // 0xBE
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

    // After the legacy managers: they keep running whether or not it starts.
    // Never destroyed (app_main never returns).
    initNvs();
    std::unique_ptr<core::Core> core_obj = startCore();
    std::unique_ptr<core::CoreLink> core_link;
    if(core_obj) {
        core_link=std::make_unique<core::CoreLink>(*core_obj,uart);
        if(!core_link->isValid()) { ESP_LOGE(TAG,"core link failed to start"); }
    }
    (void)core_obj;

    // Last, so Boot carries the final started_mask. Records from above are
    // waiting in the diag queue and go out right after Boot.
    diag::ReporterConfig diag_cfg{};
    diag_cfg.instances_ok=core_obj?static_cast<uint8_t>(core_obj->instanceCount()):0;
    diag_cfg.instances_failed=core_obj?static_cast<uint8_t>(core_obj->instancesFailed()):0;
    diag_cfg.safe_mode=core_obj && core_obj->safeMode();
    (void)started_mask;
    diag::Reporter reporter(uart, diag_cfg);
    if (!reporter.isValid()) {
        ESP_LOGE(TAG, "diag reporter failed to start; errors are on the console only");
    }

    idleForever();
}
