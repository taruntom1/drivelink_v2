#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/module_type.hpp"
#include "core/status.hpp"
#include "core/value.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core {

/// The label of an instance: at most this many characters.
inline constexpr size_t kLabelBytes = 16;

/// The loop id of the system instance, which runs in no loop.
inline constexpr uint8_t kNoLoop = 0xFF;

/// Stages of `core.InstanceStartFailed` (arg1) that the core sets itself. An
/// instance's own stages, passed to `InstanceContext::fail()`, are below
/// 0x100. Part of the diag contract (protocol/defs/diag_codes.yaml): never
/// renumber.
inline constexpr uint32_t kStartStageCreate = 0x100;  ///< create() returned nullptr
inline constexpr uint32_t kStartStageNoStage = 0x101; ///< start() returned false without fail()
inline constexpr uint32_t kStartStageBind = 0x102;    ///< a bind call was rejected
inline constexpr uint32_t kStartStageUnbound = 0x103; ///< a param, signal or input was left unbound

/// What a loop tick passes to `Instance::tick()`.
struct TickContext {
    int64_t now_us;   ///< core_os::nowUs() at the start of the tick
    float dt_s;       ///< elapsed × period, in seconds
    uint32_t elapsed; ///< periods since the previous tick (1 when on time)
};

/// A param's change hook: `fn(instance, id)`. It runs in the loop, after the
/// whole staged batch has been copied in, so it sees every new value. It must
/// be bounded and must not allocate or block (the real-time rules).
using OnChange = void (*)(void *instance, uint16_t id);

class InstanceContext;

/// The state of a configured instance (a record). Every layout entry the
/// core can hold has a record, running or not; one that isn't running has no
/// object, keeps its shadow (the values loaded from NVS) and takes param
/// writes into the shadow ("offline").
enum class InstanceState : uint8_t {
    Running,           ///< created and started; it ticks in its loop
    Failed,            ///< create() or start() failed (core.InstanceStartFailed)
    Invalid,           ///< the entry failed layout validation at boot (core.LayoutInvalid)
    DependencyMissing, ///< an instance it is wired to is not running (core.DependencyMissing)
    Offline,           ///< safe mode: nothing was created
};

/// A module instance. The core creates it at boot through its
/// `ModuleType::create`, calls `start()` once, and then `tick()`s it in its
/// loop.
class Instance {
public:
    virtual ~Instance() = default;

    /// Boot: bind every live param, signal and input to storage through `ctx`
    /// (the core checks it), read structural params with `ctx.param()`, and
    /// acquire hardware. On failure call `ctx.fail(stage, err)` and return
    /// false; the core reports `core.InstanceStartFailed` and leaves the
    /// instance out.
    virtual bool start(InstanceContext &ctx) = 0;

    /// One loop tick, in the instance's loop. In `control` the real-time
    /// rules apply.
    virtual void tick(const TickContext &t) = 0;

    // runAction comes in F3b; onArmTransition and emergencyStop in F4b.
};

namespace detail {

/// One bound item: the storage the core copies values into (params, inputs)
/// or reads from (signals).
struct Binding {
    void *ptr = nullptr;
    OnChange hook = nullptr;
    bool bound = false;
};

class InstanceTable;

/// The core's record of one configured instance. Allocated at boot.
struct InstanceRecord {
    const defs::TypeDesc *desc = nullptr;
    uint8_t type_id = 0;
    uint8_t instance = 0;
    uint8_t loop = kNoLoop;
    char label[kLabelBytes + 1] = {};
    InstanceState state = InstanceState::Running;
    /// Null unless `state` is Running.
    std::unique_ptr<Instance> object;
    /// One per item of `desc`, by index.
    std::unique_ptr<Binding[]> bindings;
    /// The value bits of each param, by item index (0 for other kinds): what
    /// the config task last applied, or the boot value. The loop never
    /// touches it.
    std::unique_ptr<uint32_t[]> shadow;

    /// Index of item `id` in `desc->items`, or `kNoItem`.
    size_t itemIndex(uint16_t id) const;
};

inline constexpr size_t kNoItem = static_cast<size_t>(-1);

} // namespace detail

/// What an instance gets in `start()`: binds, boot values and its wiring.
///
/// Each bind is checked against the descriptor: the id exists, it is of the
/// right kind, the C++ type matches the value type, and it is not bound yet.
/// A rejected bind returns false, and the instance fails to start even if it
/// ignores that. After `start()` every live param, signal and input must be
/// bound. `bindParam` writes the param's boot value into the storage.
class InstanceContext {
public:
    /// `table` resolves `ref()`; without it every ref reads as unwired.
    explicit InstanceContext(detail::InstanceRecord &record, const detail::InstanceTable *table = nullptr)
        : m_record(record), m_table(table)
    {
    }

    InstanceContext(const InstanceContext &) = delete;
    InstanceContext &operator=(const InstanceContext &) = delete;

    /// Bind param `id` to `storage`. `on_change` (optional) runs in the loop
    /// after a staged batch that changed it has been copied in.
    template <typename T> bool bindParam(uint16_t id, T *storage, OnChange on_change = nullptr)
    {
        return bind(id, defs::ItemKind::Param, valueTypeOf<T>(), storage, on_change);
    }

    /// Bind signal `id` to `storage`, which the instance updates in its tick.
    template <typename T> bool bindSignal(uint16_t id, const T *storage)
    {
        return bind(id, defs::ItemKind::Signal, valueTypeOf<T>(), const_cast<T *>(storage), nullptr);
    }

    /// Bind input `id` to `storage`, which command groups write (F4a).
    template <typename T> bool bindInput(uint16_t id, T *storage)
    {
        return bind(id, defs::ItemKind::Input, valueTypeOf<T>(), storage, nullptr);
    }

    /// The boot value bits of param `id` (for structural params), or 0 if
    /// `id` is not a param.
    uint32_t param(uint16_t id) const;

    /// The instance that wiring param `id` (a `ref` param) points at, or
    /// nullptr when it is unwired (0) or `id` is not a wiring param. The core
    /// creates dependencies first, so a wired instance exists and is running
    /// (an instance whose dependency is missing is never created). The
    /// pointer stays valid while the core lives; the module casts it to the
    /// type it expects for the ref's category.
    Instance *ref(uint16_t id) const;

    /// Record why `start()` failed. `stage` is the instance's own (below
    /// 0x100) and goes into `core.InstanceStartFailed` arg1; `err` is its
    /// esp_err. The first call wins.
    void fail(uint32_t stage, int32_t err);

    uint8_t typeId() const { return m_record.type_id; }
    uint8_t instanceId() const { return m_record.instance; }
    uint8_t loop() const { return m_record.loop; }
    const char *label() const { return m_record.label; }

    // For the core.
    bool failed() const { return m_failed; }
    uint32_t failStage() const { return m_fail_stage; }
    int32_t failErr() const { return m_fail_err; }
    /// The first rejected bind, or None.
    Detail bindError() const { return m_bind_error; }

private:
    bool bind(uint16_t id, defs::ItemKind kind, defs::ValueType type, void *storage, OnChange hook);

    detail::InstanceRecord &m_record;
    const detail::InstanceTable *m_table;
    bool m_failed = false;
    uint32_t m_fail_stage = 0;
    int32_t m_fail_err = 0;
    Detail m_bind_error = Detail::None;
};

} // namespace core
