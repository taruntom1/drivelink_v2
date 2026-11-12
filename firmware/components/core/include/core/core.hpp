#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/core_config.hpp"
#include "core/instance.hpp"
#include "core/instance_table.hpp"
#include "core/loop.hpp"
#include "core/module_type.hpp"
#include "core/param_service.hpp"
#include "core/registry.hpp"
#include "core/status.hpp"
#include "core_os/periodic_task.hpp"
#include "core_os/types.hpp"

namespace core {

/// Stages of `core.InitFailed` (arg0), reported by main. Part of the diag
/// contract (protocol/defs/diag_codes.yaml): never renumber.
inline constexpr uint32_t kInitStageRegistry = 1;
inline constexpr uint32_t kInitStageControlTask = 2;
inline constexpr uint32_t kInitStageSensorTask = 3;

/// Loop period for a rate: 1e6 / rate_hz, rounded to the nearest µs.
constexpr uint32_t periodUsFromHz(uint32_t rate_hz)
{
    return (rate_hz == 0U) ? 0U : (1000000U + (rate_hz / 2U)) / rate_hz;
}

/// The core facade: the registry, the system instance (the core's own
/// storage for the system params, bound like an instance), the two loops
/// with their tasks, the instances and the param service.
///
/// Boot sequence: construct, `init()` with the module list, `createInstance()`
/// for each instance, then `start()`. Everything is allocated by then.
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

    /// Boot only (before `start()`): create and start an instance. Checks
    /// that the type is registered (`NotFound` / `UnknownType`), that it may
    /// run in `loop` (`InvalidArgument` / `LoopMismatch`), that (type, inst)
    /// is new (`Exists`), that there is room (`NoMemory` / `Capacity`) and
    /// that the label has at most 16 characters (`BadLabel`). The label is
    /// stored; its uniqueness is checked from F3a-2. An instance whose
    /// `create` or `start` fails, or that leaves an item unbound, is reported
    /// (`core.InstanceStartFailed`) and left out (`Fault` / `StartFailed`).
    Status createInstance(uint8_t type_id, uint8_t instance, uint8_t loop, const char *label);

    /// Start both loop tasks. On failure nothing is left running, and
    /// `startStage()` / `startError()` say why (for `core.InitFailed`).
    Status start();

    /// Stop both loop tasks. Idempotent.
    void stop();

    bool isRunning() const { return m_running; }
    uint32_t startStage() const { return m_start_stage; }
    core_os::Err startError() const { return m_start_err; }

    ParamService &params() { return m_params; }
    Loop &loop(uint8_t id) { return (id == kSensorLoop) ? m_sensor : m_control; }
    const Registry &registry() const { return m_registry; }

    /// Module instances created (the system instance not counted).
    size_t instanceCount() const { return m_table.size() - (m_system_ok ? 1U : 0U); }

    /// The loop task's least free stack, in bytes (0 natively or when stopped).
    uint32_t loopStackHighWater(uint8_t id) const;

private:
    static bool applySystemParam(void *ctx, uint16_t id, uint32_t bits);
    core_os::PeriodicTask &task(uint8_t id) { return (id == kSensorLoop) ? m_sensor_task : m_control_task; }
    std::unique_ptr<detail::InstanceRecord> makeRecord(const defs::TypeDesc &desc, uint8_t type_id,
                                                       uint8_t instance, uint8_t loop, const char *label);
    Status startInstance(detail::InstanceRecord &record);

    const CoreConfig m_config;
    Registry m_registry;
    detail::InstanceTable m_table;
    Loop m_control;
    Loop m_sensor;
    ParamService m_params;
    core_os::PeriodicTask m_control_task;
    core_os::PeriodicTask m_sensor_task;
    bool m_running = false;
    bool m_started = false; ///< start() succeeded once: boot is over
    bool m_system_ok = false;
    uint32_t m_start_stage = 0;
    core_os::Err m_start_err = core_os::kOk;
};

} // namespace core
