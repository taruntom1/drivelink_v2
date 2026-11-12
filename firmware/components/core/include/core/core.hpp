#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/core_config.hpp"
#include "core/instance.hpp"
#include "core/instance_table.hpp"
#include "core/loop.hpp"
#include "core/module_type.hpp"
#include "core/param_service.hpp"
#include "core/pending_layout.hpp"
#include "core/registry.hpp"
#include "core/status.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/one_shot.hpp"
#include "core_os/periodic_task.hpp"
#include "core_os/types.hpp"

namespace core {

/// Stages of `core.InitFailed` (arg0), reported by main. Part of the diag
/// contract (protocol/defs/diag_codes.yaml): never renumber.
inline constexpr uint32_t kInitStageRegistry = 1;
inline constexpr uint32_t kInitStageControlTask = 2;
inline constexpr uint32_t kInitStageSensorTask = 3;
inline constexpr uint32_t kInitStageBoot = 4;

/// Loop period for a rate: 1e6 / rate_hz, rounded to the nearest µs.
constexpr uint32_t periodUsFromHz(uint32_t rate_hz)
{
    return (rate_hz == 0U) ? 0U : (1000000U + (rate_hz / 2U)) / rate_hz;
}

/// The core facade: the registry, the system instance (the core's own
/// storage for the system params, bound like an instance), the two loops
/// with their tasks, the instance records, the param service, the pending
/// layout and the persistence.
///
/// Boot sequence: construct, `init()` with the module list, `boot(store)`
/// (safe mode, NVS load, validation, creation in dependency order, tick
/// order), then `start()`. Everything the loops touch is allocated by then.
///
/// The layout, persistence and safe-mode ops (`addInstance` ... `exitSafeMode`)
/// are config task operations: one caller at a time, after `boot()`.
class Core {
public:
    /// Allocates the tables, the loops and the system instance.
    explicit Core(const CoreConfig &config);
    ~Core(); ///< stop(), then destroy the instances

    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;

    /// Register the module types. @return the registry's status (main
    /// reports `core.InitFailed` stage 1 with the index on failure).
    Status init(std::span<const ModuleType *const> types);

    /// Once, after `init()` and before `start()`:
    ///   1. Safe mode: the crash counter goes +1 on a crash reset, 0 on any
    ///      other; at 3 the device boots in safe mode (`core.SafeModeEntered`).
    ///   2. Load the newest complete generation from `store`, or boot with the
    ///      defaults and no instances (`core.ConfigLoadFailed` unless the
    ///      store is blank). The system params are applied.
    ///   3. Validate the layout. An invalid entry is left out
    ///      (`core.LayoutInvalid`, `layoutInvalid()`).
    ///   4. Make a record for every entry (an unknown type, a duplicate or an
    ///      entry beyond the capacity gets none). In safe mode they all stay
    ///      `Offline` and nothing is created.
    ///   5. Create and start the instances in dependency order. One that
    ///      fails is `Failed` (`core.InstanceStartFailed`), and every instance
    ///      wired to one that isn't running is `DependencyMissing`
    ///      (`core.DependencyMissing`), transitively.
    ///   6. Fill each loop in the tick order of the wiring.
    ///   7. Outside safe mode, clear the crash counter after
    ///      `crash_clear_delay_us`.
    /// `store` must outlive the core. @return Ok, or `Busy` / `NotAtBoot`
    /// (called twice, or after `start()`), or `Fault` / `StartFailed` (the
    /// system instance failed).
    Status boot(core_os::KvStore &store);

    /// Start both loop tasks. On failure nothing is left running, and
    /// `startStage()` / `startError()` say why (for `core.InitFailed`).
    Status start();

    /// Stop both loop tasks. Idempotent.
    void stop();

    // -- layout (the pending copy) ----------------------------------------------
    // Refused while armed (`Armed`) and before boot (`Busy` / `NotAtBoot`).
    // They change only the pending layout: SAVE and a reboot apply it.

    Status addInstance(uint8_t type_id, uint8_t loop, const char *label, uint8_t &instance);
    Status removeInstance(uint8_t type_id, uint8_t instance);
    Status setLabel(uint8_t type_id, uint8_t instance, const char *label);
    Status discardPending();
    /// Params as the pending layout has them (GET_PENDING).
    Status getPending(std::span<const Ref> refs, std::span<uint32_t> values) const;
    const PendingLayout &pendingLayout() const { return m_pending; }

    // -- persistence ------------------------------------------------------------

    /// Write a new generation: the pending layout if there is one, else the
    /// configured records (every record, running or not), with every
    /// `persist` param, and the system params. Refused while armed (`Armed`).
    /// A write failure answers `Fault` / `StorageFailed` and leaves the
    /// previous generation intact.
    Status save();

    /// Reload the live params of the configured instances (and the system's)
    /// from the newest complete generation, through the param service: per
    /// domain, in batches of at most 64. `NotFound` / `NoSavedConfig` when NVS
    /// holds none. A failed batch stops it; the batches before it stay.
    Status revert();

    /// Erase every core namespace (the layout and every instance). Refused
    /// while armed. The reboot is the caller's job.
    Status factoryReset();

    /// Clear the crash counter, so the next boot is a normal one. The reboot
    /// is the caller's job.
    Status exitSafeMode();

    // -- state ------------------------------------------------------------------

    bool isRunning() const { return m_running; }
    uint32_t startStage() const { return m_start_stage; }
    core_os::Err startError() const { return m_start_err; }

    bool safeMode() const { return m_safe_mode; }
    bool layoutInvalid() const { return m_layout_invalid; }
    bool configLoadedFromNvs() const { return m_config_loaded; }
    /// The pending layout was changed, or the last SAVE wrote a layout that
    /// the device is not running yet.
    bool pendingReboot() const { return m_pending.active() || m_saved_differs; }
    bool nvsDirty() const { return m_params.nvsDirty(); }

    ParamService &params() { return m_params; }
    Loop &loop(uint8_t id) { return (id == kSensorLoop) ? m_sensor : m_control; }
    const Registry &registry() const { return m_registry; }

    /// Running module instances (the system instance not counted).
    size_t instanceCount() const;
    /// Configured module instances: the records, running or not.
    size_t configuredCount() const { return m_table.size() - 1U; }
    /// Layout entries that are not running for a reason other than safe mode:
    /// Failed, Invalid, DependencyMissing, and entries left without a record.
    size_t instancesFailed() const;
    /// The record of (type, inst), or nullptr.
    const detail::InstanceRecord *findRecord(uint8_t type_id, uint8_t instance) const
    {
        return m_table.find(type_id, instance);
    }

    /// The loop task's least free stack, in bytes (0 natively or when stopped).
    uint32_t loopStackHighWater(uint8_t id) const;

private:
    static bool applySystemParam(void *ctx, uint16_t id, uint32_t bits);
    core_os::PeriodicTask &task(uint8_t id) { return (id == kSensorLoop) ? m_sensor_task : m_control_task; }
    std::unique_ptr<detail::InstanceRecord> makeRecord(const defs::TypeDesc &desc, uint8_t type_id,
                                                       uint8_t instance, uint8_t loop, const char *label,
                                                       const uint32_t *values);
    Status startInstance(detail::InstanceRecord &record);
    /// Create and start the object of a record; on failure the record is
    /// `Failed`.
    void createObject(detail::InstanceRecord &record);
    detail::InstanceRecord &systemRecord() const { return m_table.at(0); }
    Status layoutOpAllowed() const;

    const CoreConfig m_config;
    Registry m_registry;
    detail::InstanceTable m_table;
    Loop m_control;
    Loop m_sensor;
    ParamService m_params;
    PendingLayout m_pending;
    core_os::PeriodicTask m_control_task;
    core_os::PeriodicTask m_sensor_task;
    core_os::OneShotTimer m_crash_clear;
    core_os::KvStore *m_store = nullptr;
    uint32_t m_newest_gen = 0; ///< the newest generation in NVS; SAVE writes a newer one
    /// The complete generation in NVS (the one loaded or last saved), 0 if
    /// none. SAVE never writes its slot, so a torn SAVE can always fall back.
    uint32_t m_complete_gen = 0;
    size_t m_left_out = 0;     ///< layout entries with no record
    bool m_running = false;
    bool m_started = false; ///< start() succeeded once
    bool m_booted = false;
    bool m_system_ok = false;
    bool m_safe_mode = false;
    bool m_layout_invalid = false;
    bool m_config_loaded = false;
    bool m_saved_differs = false; ///< the last SAVE wrote the pending layout
    uint32_t m_start_stage = 0;
    core_os::Err m_start_err = core_os::kOk;
};

} // namespace core
