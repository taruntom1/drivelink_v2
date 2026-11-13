#pragma once

#include "core/source_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/capture_service.hpp"
#include "core/core_config.hpp"
#include "core/group_service.hpp"
#include "core/instance.hpp"
#include "core/instance_table.hpp"
#include "core/loop.hpp"
#include "core/module_type.hpp"
#include "core/param_service.hpp"
#include "core/pending_layout.hpp"
#include "core/registry.hpp"
#include "core/status.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/one_shot.hpp"
#include "core_os/periodic_task.hpp"
#include "core_os/periodic_timer.hpp"
#include "core_os/types.hpp"

namespace core {

/// Stages of `core.InitFailed` (arg0), reported by main. Part of the diag
/// contract (protocol/defs/diag_codes.yaml): never renumber.
inline constexpr uint32_t kInitStageRegistry = 1;
inline constexpr uint32_t kInitStageLoopTask = 2; // stage 3 retired
inline constexpr uint32_t kInitStageBoot = 4;

/// Loop period for a rate: 1e6 / rate_hz, rounded to the nearest µs.
constexpr uint32_t periodUsFromHz(uint32_t rate_hz)
{
    return (rate_hz == 0U) ? 0U : (1000000U + (rate_hz / 2U)) / rate_hz;
}

/// The core facade: the registry, the system instance (the core's own
/// storage for the system params, bound like an instance), the configurable loops
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
    /// Allocates the tables, loop slots and system instance; boot creates the loops.
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

    /// Release the loop tick gates after boot. Safe mode has no loop task.
    /// Idempotent while running; a stopped Core must be reconstructed.
    Status start();

    /// Stop every loop task. Idempotent.
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
    Status runAction(Ref ref, const ActionArgs &args, ActionOut &out);
    Status arm();
    void disarm(DisarmReason reason = DisarmReason::Explicit) { m_arm.disarm(reason); }
    Status acknowledgeFault();
    ArmStatus armStatus() const { return m_arm.status(); }
    DisarmReason lastDisarmReason() const { return m_arm.reason(); }
    bool blocksArm() const { return core_os::diagBlocksArm(); }
    // Timer-owned. Public for deterministic native tests; never call in
    // parallel with its timer, or before boot has fixed the loop table.
    void monitor(int64_t now_us);

    // -- state ------------------------------------------------------------------

    bool isRunning() const { return m_running; }

    bool safeMode() const { return m_safe_mode; }
    bool layoutInvalid() const { return m_layout_invalid; }
    bool configLoadedFromNvs() const { return m_config_loaded; }
    /// The pending layout was changed, or the last SAVE wrote a layout that
    /// the device is not running yet.
    bool pendingReboot() const { return m_pending.active() || m_saved_differs; }
    bool nvsDirty() const { return m_params.nvsDirty(); }

    ParamService &params() { return m_params; }
    GroupService &groups() { return m_groups; }
    CaptureService &capture() { return m_capture; }
    Loop &loop(uint8_t id) { return *findLoop(id); } ///< known running loop only
    Loop *findLoop(uint8_t id) const;
    Loop *loopAt(size_t slot) const { return slot < m_config.max_loops ? m_loops[slot] : nullptr; }
    size_t loopCapacity() const { return m_config.max_loops; }
    size_t loopCount() const;
    const Registry &registry() const { return m_registry; }

    /// Running layout instances, loops included (system excluded).
    size_t instanceCount() const;
    /// Configured layout instances, loops included: the records, running or not.
    size_t configuredCount() const { return m_table.size() - 1U; }
    const detail::InstanceRecord *recordAt(size_t index) const {
        return index < configuredCount() ? &m_table.at(index+1) : nullptr;
    }
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
    static bool applySystemParam(void *ctx, const Ref &ref, uint32_t bits);
    size_t loopSlot(uint8_t id) const;
    void createLoop(detail::InstanceRecord &record);
    std::unique_ptr<detail::InstanceRecord> makeRecord(const defs::TypeDesc &desc, uint8_t type_id,
                                                       uint8_t instance, uint8_t loop, const char *label,
                                                       const uint32_t *values);
    Status startInstance(detail::InstanceRecord &record);
    /// Create and start the object of a record; on failure the record is
    /// `Failed`.
    void createObject(detail::InstanceRecord &record);
    detail::InstanceRecord &systemRecord() const { return m_table.at(0); }
    Status layoutOpAllowed() const;
    void initializeLimits(detail::InstanceRecord &record);

    const CoreConfig m_config;
    ArmState m_arm;
    Registry m_registry;
    detail::InstanceTable m_table;
    std::array<Loop *, kMaxLoops> m_loops{};
    std::array<std::unique_ptr<Loop>, kMaxLoops> m_owned_loops;
    std::unique_ptr<core_os::PeriodicTask[]> m_tasks;
    std::array<std::array<char, 16>, kMaxLoops> m_task_names{};
    ParamService m_params;
    GroupService m_groups;
    CaptureService m_capture;
    SourceService m_sources;
    PendingLayout m_pending;
    core_os::OneShotTimer m_crash_clear;
    core_os::PeriodicTimer m_monitor;
    std::atomic<uint32_t> m_watchdog_us{200000};
    struct Heartbeat { uint32_t value = 0; int64_t advanced_us = 0; bool stalled = false; };
    std::array<Heartbeat, kMaxLoops> m_heartbeats{};
    std::unique_ptr<Instance *[]> m_actuators;
    size_t m_actuator_count = 0;
    bool m_observing = false;
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
};

} // namespace core
