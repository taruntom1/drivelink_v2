// main.cpp - DriveLink v2 entry point.
//
// Startup order: diag, UartComm, NVS, the core (init, boot from NVS, start)
// and its CoreLink, then the diag Reporter. A core that fails to start is
// reported (core.InitFailed) and left out; the link and diag keep running, so
// the host can still connect and ask what went wrong. app_main never returns.

#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>

#include "core/core.hpp"
#include "core/core_link.hpp"
#include "mod_mock/modules.hpp"
#include "mod_motor/modules.hpp"
#include "core/core_config.hpp"
#include "core/module_type.hpp"
#include "core/registry.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/bench_affinity.hpp"
#include "diag.hpp"
#include "diag_reporter.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/types.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mod_imu_bno055/modules.hpp"
#include "uart_comm.hpp"

static const char *const TAG = "main";

namespace {

#if CONFIG_DRIVELINK_STACK_LOG_PERIOD_S > 0 && CONFIG_DIAG_CONSOLE_LOG
#define DRIVELINK_LOG_STACKS 1

// The firmware's tasks, by the names they are created with. A name with no
// task (one that did not start) is skipped.
// esp_timer runs the timer callbacks (the loop timers, the safety monitor and
// the IMU sampling timer).
constexpr std::array kLoggedTasks = {
    "main", "uart_rx", "uart_tx", "diag", "esp_timer", "core_cfg",
};

/// Log each task's stack high-water mark: the least free stack, in bytes,
/// since the task started. Used to size the task stacks.
/// uxTaskGetStackHighWaterMark() scans the stack without the kernel lock, and
/// xTaskGetHandle() holds it only to walk the task lists, so this doesn't
/// stall the control loop on the other core. (uxTaskGetSystemState() would
/// scan every stack inside the kernel's critical section.)
void logStackHighWater(const core::Core *core = nullptr)
{
#if CONFIG_CORE_BENCH_CRASH
    core_os::logBenchTaskAffinities();
#endif
    for (const char *name : kLoggedTasks) {
        TaskHandle_t handle = xTaskGetHandle(name);
        if (handle != nullptr) {
            ESP_LOGI(TAG, "Stack high-water: %-15s %5u bytes free", name,
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(handle)));
        }
    }
    if (core) {
        for (size_t i = 0; i < core->loopCapacity(); ++i) {
            const auto *loop = core->loopAt(i); if (!loop) { continue; }
            char name[16]; std::snprintf(name, sizeof(name), "core_loop_%u", loop->id());
            ESP_LOGI(TAG, "Stack high-water: %-15s %5u bytes free", name,
                     static_cast<unsigned>(core->loopStackHighWater(loop->id())));
        }
    }
}
#endif

/// The module types the core can instantiate. F3b adds the first ones. Each
/// desc must be its generated kTypeTable entry (checked in startCore()).
const std::array<const core::ModuleType *, 8> kModuleTypes{
    mod_mock::kTypes[0], mod_mock::kTypes[1], mod_mock::kTypes[2], mod_mock::kTypes[3], mod_motor::kTypes[0],
    mod_motor::kHardwareTypes[0], mod_motor::kHardwareTypes[1], &mod_imu_bno055::kType};

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

/// Build, boot and start the core: the registry, the layout and params from
/// NVS (or the defaults), the instances (none until F3b brings module types)
/// and the configured loops. The Core lives on the heap: app_main's stack is small. On
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
        DIAG_ERROR(diag::codes::core::kInitFailed, ESP_ERR_INVALID_STATE, core::kInitStageBoot, 0,
                   "Core: start refused (result %u, detail %u)", static_cast<unsigned>(st.result),
                   static_cast<unsigned>(st.detail));
        return nullptr;
    }
    ESP_LOGI(TAG, "Core started: %u instances (%u failed, %u configured); safe mode %s; config %s; %u loops",
             static_cast<unsigned>(c->instanceCount()), static_cast<unsigned>(c->instancesFailed()),
             static_cast<unsigned>(c->configuredCount()), c->safeMode() ? "ON" : "off",
             c->configLoadedFromNvs() ? "loaded from NVS" : "factory", static_cast<unsigned>(c->loopCount()));
    for (size_t i = 0; i < c->loopCapacity(); ++i) {
        const auto *loop = c->loopAt(i); if (!loop) { continue; }
        ESP_LOGI(TAG, "Loop %u (%s): %u us, realtime %u", loop->id(), loop->name(),
                 static_cast<unsigned>(loop->periodUs()), loop->realtime());
    }
    return c;
}

[[noreturn]] void idleForever(const core::Core *core = nullptr)
{
    (void)core;
#if DRIVELINK_LOG_STACKS
    constexpr TickType_t kPeriod = pdMS_TO_TICKS(CONFIG_DRIVELINK_STACK_LOG_PERIOD_S * 1000);
    while (true) {
        vTaskDelay(kPeriod);
        logStackHighWater(core);
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

    // Core owns every protocol-facing service.
    initNvs();
    std::unique_ptr<core::Core> core_obj = startCore();
    std::unique_ptr<core::CoreLink> core_link;
    if(core_obj) {
        core_link=std::make_unique<core::CoreLink>(*core_obj,uart);
        if(!core_link->isValid()) { ESP_LOGE(TAG,"core link failed to start"); }
    }
    (void)core_obj;

    // Last, so Boot carries the final instance counts. Records from above are
    // waiting in the diag queue and go out right after Boot.
    diag::ReporterConfig diag_cfg{};
    diag_cfg.instances_ok=core_obj?static_cast<uint8_t>(core_obj->instanceCount()):0;
    diag_cfg.instances_failed=core_obj?static_cast<uint8_t>(core_obj->instancesFailed()):0;
    diag_cfg.safe_mode=core_obj && core_obj->safeMode();
    diag::Reporter reporter(uart, diag_cfg);
    if (!reporter.isValid()) {
        ESP_LOGE(TAG, "diag reporter failed to start; errors are on the console only");
    }

    idleForever(core_obj.get());
}
