#include "core/core.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include "core/loop_instance.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"
#include <utility>

#include "core/layout_validation.hpp"
#include "core/persistence.hpp"
#include "core/safe_mode.hpp"
#include "core/value.hpp"
#include "core/wiring_order.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

namespace core {

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace diag_codes = drivelink::protocol::defs::diag::codes;

constexpr const char *kCrashClearName = "core_crash_clr";

/// The core's own storage for the system params. It runs in no loop: the
/// param service applies system params on the config task.
class SystemInstance final : public Instance {
public:
    bool start(InstanceContext &ctx) override
    {
        ctx.bindParam(sys::kStatusRateHz, &m_status_rate_hz);
        ctx.bindParam(sys::kWatchdogMs, &m_watchdog_ms);
        ctx.bindParam(sys::kLimitsLoops, &m_limits[0]);
        ctx.bindParam(sys::kLimitsInstances, &m_limits[1]);
        ctx.bindParam(sys::kLimitsSubGroups, &m_limits[2]);
        ctx.bindParam(sys::kLimitsCmdGroups, &m_limits[3]);
        ctx.bindParam(sys::kLimitsGroupEntries, &m_limits[4]);
        ctx.bindParam(sys::kLimitsLinkValues, &m_limits[5]);
        return true;
    }

    void tick(const TickContext &t) override { (void)t; }

private:
    uint16_t m_status_rate_hz = 10;
    uint16_t m_watchdog_ms = 200;
    std::array<uint16_t, 6> m_limits{};
};

void reportStartFailed(uint8_t type,uint8_t instance,core_os::Err err,uint32_t stage) {
    for(const auto &code:defs::diag::kCodeTable) {
        if(static_cast<uint8_t>(code.source)==type && code.code==1 && type>=0x20) {
            core_os::reportDiag(code,err,stage,0,instance);return;
        }
    }
    core_os::reportDiag(diag_codes::core::kInstanceStartFailed,err,(static_cast<uint32_t>(type)<<8U)|instance,stage,instance);
}

uint32_t instanceArg(uint8_t type_id, uint8_t instance)
{
    return (static_cast<uint32_t>(type_id) << 8U) | instance;
}

/// The OneShotTimer callback, on the esp_timer task: 30 s of normal uptime
/// clear the crash counter.
void clearCrashCount(void *ctx)
{
    (void)ctx;
    core_os::storeCrashCount(0);
}

/// A live param that REVERT reloads: persisted, writable, not reboot_required.
bool revertable(const defs::ItemDesc &item)
{
    return item.kind == defs::ItemKind::Param && defs::hasFlag(item, defs::ItemFlag::Persist) &&
           !defs::hasFlag(item, defs::ItemFlag::ReadOnly) && !defs::hasFlag(item, defs::ItemFlag::RebootRequired);
}

CoreConfig normalized(CoreConfig config)
{
    config.max_loops = std::clamp<uint8_t>(config.max_loops, 1, kMaxLoops);
    config.max_instances = std::clamp<uint16_t>(config.max_instances, 1, 64);
    config.link_values = std::clamp<uint16_t>(config.link_values, 1, 64);
    return config;
}

} // namespace

Core::Core(const CoreConfig &config)
    : m_config(normalized(config)),
      m_table(static_cast<size_t>(m_config.max_instances) + m_config.max_loops + 1U),
      m_tasks(std::make_unique<core_os::PeriodicTask[]>(m_config.max_loops)),
      m_params(m_table, std::span(m_loops.data(), m_config.max_loops), &Core::applySystemParam, this),
      m_groups(m_table, std::span(m_loops.data(), m_config.max_loops), m_config.subscription_groups,
               m_config.command_groups, m_config.group_entries),
      m_pending(m_registry, m_table, ValidationConfig{m_config.gpio_allowed_mask, m_config.max_instances, m_config.max_loops, m_config.link_values})
{
    m_params.setPendingLayout(&m_pending);
    m_params.useArmWord(&m_arm.word);
    m_actuators = std::make_unique<Instance *[]>(m_config.max_instances);
    std::unique_ptr<detail::InstanceRecord> record =
        makeRecord(sys::kDesc, drivelink::protocol::defs::kSystemTypeId, 0, kNoLoop, "system", nullptr);
    record->object = std::make_unique<SystemInstance>();
    initializeLimits(*record);
    m_system_ok = startInstance(*record).ok();
    if (m_system_ok) {
        (void)m_table.add(std::move(record)); // the table has room: it was just made
    }
}

void Core::initializeLimits(detail::InstanceRecord &record) {
    const std::array<std::pair<uint16_t, uint32_t>, 6> values{{
        {sys::kLimitsLoops, m_config.max_loops}, {sys::kLimitsInstances, m_config.max_instances},
        {sys::kLimitsSubGroups, static_cast<uint32_t>(m_groups.capacity(GroupKind::Subscription))},
        {sys::kLimitsCmdGroups, static_cast<uint32_t>(m_groups.capacity(GroupKind::Command))},
        {sys::kLimitsGroupEntries, m_groups.entryCapacity()}, {sys::kLimitsLinkValues, m_config.link_values}}};
    for (const auto &[id, value] : values) { record.shadow[record.itemIndex(id)] = value; }
}

Core::~Core()
{
    stop();
    m_crash_clear.cancel();
}

Status Core::init(std::span<const ModuleType *const> types)
{
    if (!m_system_ok || m_config.max_loops == 0 || m_config.max_loops > kMaxLoops) {
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    return m_registry.init(types);
}

std::unique_ptr<detail::InstanceRecord> Core::makeRecord(const defs::TypeDesc &desc, uint8_t type_id,
                                                         uint8_t instance, uint8_t loop, const char *label,
                                                         const uint32_t *values)
{
    auto record = std::make_unique<detail::InstanceRecord>();
    record->desc = &desc;
    record->type_id = type_id;
    record->instance = instance;
    record->loop = loop;
    const size_t label_len = std::min(std::strlen(label), kLabelBytes);
    std::memcpy(record->label, label, label_len);
    record->label[label_len] = '\0';
    const size_t n = desc.items.size();
    record->bindings = std::make_unique<detail::Binding[]>(n);
    record->shadow = std::make_unique<uint32_t[]>(n);
    for (size_t i = 0; i < n; ++i) {
        const defs::ItemDesc &item = desc.items[i];
        const uint32_t def = (item.kind == defs::ItemKind::Param) ? item.default_bits : 0U;
        record->shadow[i] = (values != nullptr) ? values[i] : def;
    }
    return record;
}

Status Core::startInstance(detail::InstanceRecord &record)
{
    InstanceContext ctx(record, &m_table);
    const bool started = record.object->start(ctx);

    uint32_t stage = 0;
    int32_t err = 0;
    if (ctx.failed()) {
        stage = ctx.failStage();
        err = ctx.failErr();
    } else if (ctx.bindError() != Detail::None) {
        stage = kStartStageBind;
    } else if (!started) {
        stage = kStartStageNoStage;
    } else {
        for (size_t i = 0; i < record.desc->items.size(); ++i) {
            if (defs::hasFlag(record.desc->items[i], defs::ItemFlag::Source)) {
                bool source_bound = false;
                for (const auto &source : record.sources) { source_bound |= source.param_index == i; }
                if (!source_bound) { stage = kStartStageUnbound; break; }
            }
            if (record.desc->items[i].kind != defs::ItemKind::Action && !record.bindings[i].bound) {
                stage = kStartStageUnbound;
                break;
            }
        }
    }
    if (stage == 0U && (ctx.failed() || !started)) {
        stage = kStartStageNoStage; // fail() was called with stage 0
    }
    if (stage != 0U) {
        reportStartFailed(record.type_id,record.instance,err,
                            stage);
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    return Status::success();
}

void Core::createObject(detail::InstanceRecord &record)
{
    record.state = InstanceState::Failed;
    const ModuleType *type = m_registry.find(record.type_id);
    if (type == nullptr) {
        return; // not reachable: validation checked the type
    }
    const InstanceConfig cfg{&type->desc, record.type_id, record.instance, record.loop, record.label};
    record.object.reset(type->create(cfg));
    if (!record.object) {
        reportStartFailed(record.type_id,record.instance,0,
                            kStartStageCreate);
        return;
    }
    if (!startInstance(record).ok()) {
        record.object.reset();
        // Forget the binds into the freed object; the shadow (its config) stays.
        record.bindings = std::make_unique<detail::Binding[]>(record.desc->items.size());
        return;
    }
    record.state = InstanceState::Running;
}

Status Core::boot(core_os::KvStore &store)
{
    if (!m_system_ok) {
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    if (m_booted || m_started) {
        return Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    m_booted = true;
    m_store = &store;

    // 1. Safe mode.
    const SafeModeDecision safe = decideSafeMode(core_os::loadCrashCount(), core_os::resetWasCrash());
    core_os::storeCrashCount(safe.counter);
    m_safe_mode = safe.safe_mode;
    if (m_safe_mode) {
        core_os::reportDiag(diag_codes::core::kSafeModeEntered, 0, safe.counter, 0);
    }

    // 2. Load. The system params apply now (the loop periods too).
    persist::LoadedConfig cfg = persist::loadConfig(store);
    m_newest_gen = cfg.newest_gen;
    if (cfg.complete) {
        m_config_loaded = true;
        m_complete_gen = cfg.gen;
        detail::InstanceRecord &system = systemRecord();
        persist::applyPairs(*system.desc, cfg.system, system.shadow.get());
        initializeLimits(system);
        for (size_t i = 0; i < system.desc->items.size(); ++i) {
            const defs::ItemDesc &item = system.desc->items[i];
            if (item.kind == defs::ItemKind::Param && system.bindings[i].bound) {
                storeValue(item.type, system.bindings[i].ptr, system.shadow[i]);
                (void)applySystemParam(this, Ref{system.type_id, system.instance, item.id}, system.shadow[i]); // the tasks aren't running: cannot fail
            }
        }
    } else if (cfg.found_any) {
        core_os::reportDiag(diag_codes::core::kConfigLoadFailed, 0, cfg.newest_gen, cfg.cause);
    }

    if (!cfg.complete) {
        namespace lp = defs::loop;
        auto factory = [&](uint8_t id, const char *name, bool realtime) {
            persist::StoredInstance entry{};
            entry.type_id = defs::kLoopTypeId; entry.instance = id; entry.loop = kNoLoop;
            std::strncpy(entry.label, name, kLabelBytes);
            entry.pairs = {{lp::kRateHz, realtime ? 1000U : 100U}, {lp::kRealtime, realtime ? 1U : 0U},
                           {lp::kCore, realtime ? 1U : 0U}, {lp::kPriority, realtime ? 20U : 10U}};
            cfg.instances.push_back(std::move(entry));
        };
        factory(kFactoryControlLoop, "control", true);
        if (m_config.max_loops > 1) { factory(kFactorySensorLoop, "sensor", false); }
    }

    // 3. Validate.
    const size_t n = cfg.instances.size();
    std::vector<std::vector<uint32_t>> values(n);
    std::vector<LayoutEntry> entries(n);
    for (size_t i = 0; i < n; ++i) {
        const persist::StoredInstance &s = cfg.instances[i];
        const ModuleType *type = m_registry.find(s.type_id);
        const defs::TypeDesc *desc = (type != nullptr) ? &type->desc : nullptr;
        if (desc != nullptr) {
            values[i].resize(desc->items.size());
            persist::applyPairs(*desc, s.pairs, values[i].data());
        }
        entries[i] = LayoutEntry{desc, s.type_id, s.instance, s.loop, s.label, values[i].data()};
    }
    std::vector<Status> status(n);
    (void)validateLayout(entries, ValidationConfig{m_config.gpio_allowed_mask, m_config.max_instances, m_config.max_loops, m_config.link_values}, status);

    // 4. Records.
    std::vector<detail::InstanceRecord *> records(n, nullptr);
    for (size_t i = 0; i < n; ++i) {
        const Status &st = status[i];
        const LayoutEntry &e = entries[i];
        if (!st.ok() && st.detail != Detail::DependencyMissing) {
            m_layout_invalid = true;
            core_os::reportDiag(diag_codes::core::kLayoutInvalid, 0, instanceArg(e.type_id, e.instance),
                                static_cast<uint32_t>(st.detail));
        }
        if (e.desc == nullptr || st.result == Result::Exists || st.detail == Detail::Capacity) {
            ++m_left_out; // nothing to hold it in
            continue;
        }
        std::unique_ptr<detail::InstanceRecord> record =
            makeRecord(*e.desc, e.type_id, e.instance, e.loop, e.label, e.values);
        if (m_safe_mode) {
            record->state = InstanceState::Offline;
        } else if (st.ok()) {
            record->state = InstanceState::Failed; // until it is created
        } else {
            record->state =
                (st.detail == Detail::DependencyMissing) ? InstanceState::DependencyMissing : InstanceState::Invalid;
        }
        records[i] = record.get();
        (void)m_table.add(std::move(record)); // room: an entry past the capacity got no record
    }
    if (m_safe_mode) {
        return Status::success(); // no instance, and the crash counter stays
    }

    // Establish loop tasks before starting their dependent modules. Their
    // callbacks stay gated until start(), after every tick list is complete.
    for (size_t i = 0; i < n; ++i) {
        if (records[i] && status[i].ok() && records[i]->type_id == defs::kLoopTypeId) {
            createLoop(*records[i]);
        }
    }

    // 5. Create, dependencies first.
    std::vector<bool> include(n, false);
    for (size_t i = 0; i < n; ++i) {
        include[i] = records[i] != nullptr && (status[i].ok() || status[i].detail == Detail::DependencyMissing);
    }
    for (const size_t i : creationOrder(entries, include)) {
        detail::InstanceRecord &record = *records[i];
        if (record.type_id == defs::kLoopTypeId) { continue; }
        const auto *owner = m_table.find(defs::kLoopTypeId, record.loop);
        if (!owner || owner->state != InstanceState::Running) {
            record.state = InstanceState::DependencyMissing;
            core_os::reportDiag(diag_codes::core::kDependencyMissing, 0, instanceArg(record.type_id, record.instance),
                                instanceArg(defs::kLoopTypeId, record.loop));
            continue;
        }
        uint32_t missing = 0;
        bool is_missing = false;
        for (const RefLink &link : refLinks(entries, i)) {
            const detail::InstanceRecord *target = (link.target != kNoEntry) ? records[link.target] : nullptr;
            if (target == nullptr || target->state != InstanceState::Running) {
                is_missing = true;
                missing = link.value & 0xFFFFU;
                break;
            }
        }
        if (is_missing) {
            record.state = InstanceState::DependencyMissing;
            core_os::reportDiag(diag_codes::core::kDependencyMissing, 0, instanceArg(record.type_id, record.instance),
                                missing);
            continue;
        }
        createObject(record);
    }

    // 6. Fill the loops in tick order.
    for (size_t i = 0; i < n; ++i) {
        include[i] = records[i] != nullptr && records[i]->type_id != defs::kLoopTypeId && records[i]->state == InstanceState::Running;
    }
    for (const size_t i : tickOrder(entries, include)) {
        detail::InstanceRecord &record = *records[i];
        (void)loop(record.loop).addInstance(record.object.get()); // sized for max_instances
        loop(record.loop).addInputs(record);
        if (defs::inCategory(record.desc->name, "actuator")) {
            m_actuators[m_actuator_count++] = record.object.get();
        }
    }

    m_sources.build(m_table);

    // 7. Clear the crash counter after a while of normal running.
    (void)m_crash_clear.start(kCrashClearName, m_config.crash_clear_delay_us, &clearCrashCount, nullptr);
    return Status::success();
}

void Core::createLoop(detail::InstanceRecord &record)
{
    createObject(record);
    if (record.state != InstanceState::Running) { return; }
    auto &instance = static_cast<LoopInstance &>(*record.object);
    size_t slot = 0;
    while (slot < m_config.max_loops && m_loops[slot]) { ++slot; }
    if (slot == m_config.max_loops) { record.state = InstanceState::Failed; return; }
    m_owned_loops[slot] = std::make_unique<Loop>(record.instance, record.label, instance.realtime,
        m_config.max_instances, periodUsFromHz(instance.rate), &instance.signals);
    m_loops[slot] = m_owned_loops[slot].get();
    m_loops[slot]->setGroups(&m_groups);
    m_loops[slot]->setSources(&m_sources);
    m_loops[slot]->setArmState(&m_arm);
    std::snprintf(m_task_names[slot].data(), m_task_names[slot].size(), "core_loop_%u", record.instance);
    const auto err = m_tasks[slot].start(m_task_names[slot].data(), instance.stack, instance.priority, instance.core,
        m_loops[slot]->periodUs(), &Loop::taskEntry, m_loops[slot], instance.realtime);
    if (err != core_os::kOk) {
        record.state = InstanceState::Failed;
        m_loops[slot] = nullptr;
        m_owned_loops[slot].reset();
        core_os::reportDiag(diag_codes::core::kInitFailed, err, kInitStageLoopTask, record.instance);
    }
}

Status Core::start()
{
    if (!m_booted) { return Status::fail(Result::Busy, Detail::NotAtBoot); }
    if (m_started) {
        return m_running ? Status::success() : Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    const auto now = core_os::nowUs();
    for (size_t i = 0; i < m_config.max_loops; ++i) {
        m_heartbeats[i].advanced_us = now;
        if (m_loops[i]) { m_heartbeats[i].value = m_loops[i]->heartbeat(); }
    }
    core_os::setDiagDisarmHandler(this, [](void *ctx, core_os::DiagCode code) {
        auto *self = static_cast<Core *>(ctx);
        const bool watchdog = code.source == diag_codes::core::kWatchdogTimeout.source &&
                              code.code == diag_codes::core::kWatchdogTimeout.code;
        const bool stalled = code.source == diag_codes::core::kLoopStalled.source &&
                             code.code == diag_codes::core::kLoopStalled.code;
        self->m_arm.disarm(watchdog ? DisarmReason::Watchdog : stalled ? DisarmReason::LoopStalled : DisarmReason::Fault, !watchdog);
    });
    m_observing = true;
    if (core_os::diagDisarmsActive()) { m_arm.disarm(DisarmReason::Fault, true); }
    if (!m_safe_mode && m_monitor.start("core_monitor", 10000, [](void *ctx) {
        static_cast<Core *>(ctx)->monitor(core_os::nowUs());
    }, this) != core_os::kOk) {
        core_os::setDiagDisarmHandler(nullptr, nullptr); m_observing = false;
        return Status::fail(Result::Fault, Detail::MonitorUnavailable);
    }
    for (size_t i = 0; i < m_config.max_loops; ++i) {
        if (m_loops[i]) { m_loops[i]->enable(true); }
    }
    m_running = true;
    m_started = true;
    return Status::success();
}

void Core::stop()
{
    m_arm.word.fetch_or(ArmState::kStopping, std::memory_order_acq_rel);
    m_arm.disarm(DisarmReason::Shutdown);
    m_monitor.stop();
    for (size_t i = 0; i < m_actuator_count; ++i) { m_actuators[i]->emergencyStop(); }
    for (size_t i = 0; i < m_config.max_loops; ++i) {
        if (m_loops[i]) { m_loops[i]->enable(false); }
        m_tasks[i].stop();
    }
    for (size_t i = 0; i < m_actuator_count; ++i) { m_actuators[i]->emergencyStop(); }
    m_running = false;
    if (m_observing) { core_os::setDiagDisarmHandler(nullptr, nullptr); m_observing = false; }
}

size_t Core::loopSlot(uint8_t id) const
{
    for (size_t i = 0; i < m_config.max_loops; ++i) {
        if (m_loops[i] && m_loops[i]->id() == id) { return i; }
    }
    return m_config.max_loops;
}

Loop *Core::findLoop(uint8_t id) const
{
    const size_t slot = loopSlot(id);
    return slot < m_config.max_loops ? m_loops[slot] : nullptr;
}

size_t Core::loopCount() const
{
    size_t count = 0;
    for (size_t i = 0; i < m_config.max_loops; ++i) { count += m_loops[i] != nullptr ? 1U : 0U; }
    return count;
}

// -- layout ----------------------------------------------------------------------

Status Core::layoutOpAllowed() const
{
    if (!m_booted) {
        return Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    if (m_params.armed()) {
        return Status::fail(Result::Armed, Detail::None);
    }
    return Status::success();
}

Status Core::addInstance(uint8_t type_id, uint8_t loop_id, const char *label, uint8_t &instance)
{
    Status st = layoutOpAllowed();
    if (st.ok()) {
        st = m_pending.addInstance(type_id, loop_id, label, instance);
    }
    if (st.ok()) {
        m_params.setNvsDirty(true);
    }
    return st;
}

Status Core::removeInstance(uint8_t type_id, uint8_t instance)
{
    Status st = layoutOpAllowed();
    if (st.ok()) {
        st = m_pending.removeInstance(type_id, instance);
    }
    if (st.ok()) {
        m_params.setNvsDirty(true);
    }
    return st;
}

Status Core::setLabel(uint8_t type_id, uint8_t instance, const char *label)
{
    Status st = layoutOpAllowed();
    if (st.ok()) {
        st = m_pending.setLabel(type_id, instance, label);
    }
    if (st.ok()) {
        m_params.setNvsDirty(true);
    }
    return st;
}

Status Core::discardPending()
{
    const Status st = layoutOpAllowed();
    if (st.ok()) {
        m_pending.discard();
    }
    return st;
}

Status Core::getPending(std::span<const Ref> refs, std::span<uint32_t> values) const
{
    return m_pending.get(refs, values);
}

// -- persistence -----------------------------------------------------------------

Status Core::save()
{
    if (m_store == nullptr) {
        return Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    if (m_params.armed()) {
        return Status::fail(Result::Armed, Detail::None);
    }

    std::vector<persist::StoredInstance> items;
    auto add = [&](uint8_t type_id, uint8_t instance, uint8_t loop_id, const char *label,
                   std::vector<ParamPair> pairs) {
        persist::StoredInstance s{};
        s.type_id = type_id;
        s.instance = instance;
        s.loop = loop_id;
        std::memcpy(s.label, label, std::min(std::strlen(label), kLabelBytes));
        s.pairs = std::move(pairs);
        items.push_back(std::move(s));
    };
    if (m_pending.active()) {
        for (const PendingEntry &e : m_pending.entries()) {
            add(e.type_id, e.instance, e.loop, e.label,
                persist::persistPairs(*e.desc, [&e](size_t k) { return PendingLayout::valueAt(e, k); }));
        }
    } else {
        for (size_t i = 1; i < m_table.size(); ++i) { // 0 is the system
            const detail::InstanceRecord &r = m_table.at(i);
            add(r.type_id, r.instance, r.loop, r.label,
                persist::persistPairs(*r.desc, [&r](size_t k) { return r.shadow[k]; }));
        }
    }
    const detail::InstanceRecord &system = systemRecord();
    const std::vector<ParamPair> system_pairs =
        persist::persistPairs(*system.desc, [&system](size_t k) { return system.shadow[k]; });

    // Newer than anything in NVS, and never in the slot of the complete
    // generation: after an incomplete newer one (a torn earlier SAVE),
    // newest + 1 would land on the complete one's slot.
    uint32_t gen = m_newest_gen + 1U;
    if (m_complete_gen != 0U && (gen & 1U) == (m_complete_gen & 1U)) {
        ++gen;
    }
    const Status st = persist::saveConfig(*m_store, gen, items, system_pairs);
    if (!st.ok()) {
        // Some blobs of `gen` may be written: the next SAVE goes past it.
        m_newest_gen = gen;
        return st;
    }
    m_newest_gen = gen;
    m_complete_gen = gen;
    m_params.setNvsDirty(false);
    m_saved_differs = m_pending.active();
    return Status::success();
}

Status Core::revert()
{
    if (m_store == nullptr) {
        return Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    const persist::LoadedConfig cfg = persist::loadConfig(*m_store);
    if (!cfg.complete) {
        return Status::fail(Result::NotFound, Detail::NoSavedConfig);
    }

    // One list per domain: system/live loop params, each actual loop, offline.
    std::vector<std::vector<SetEntry>> lists(static_cast<size_t>(m_config.max_loops) + 2U);
    auto addRecord = [&](const detail::InstanceRecord &r, std::span<const ParamPair> pairs) {
        std::vector<uint32_t> v(r.desc->items.size());
        persist::applyPairs(*r.desc, pairs, v.data());
        size_t list = static_cast<size_t>(m_config.max_loops) + 1U;
        if (r.type_id == defs::kSystemTypeId ||
            (r.type_id == defs::kLoopTypeId && r.state == InstanceState::Running)) {
            list = 0;
        } else if (r.state == InstanceState::Running) {
            list = loopSlot(r.loop) + 1U;
        }
        for (size_t k = 0; k < r.desc->items.size(); ++k) {
            if (revertable(r.desc->items[k])) {
                lists[list].push_back(SetEntry{Ref{r.type_id, r.instance, r.desc->items[k].id}, v[k]});
            }
        }
    };
    addRecord(systemRecord(), cfg.system);
    for (size_t i = 1; i < m_table.size(); ++i) {
        const detail::InstanceRecord &r = m_table.at(i);
        for (const persist::StoredInstance &s : cfg.instances) {
            if (s.type_id == r.type_id && s.instance == r.instance) {
                addRecord(r, s.pairs);
                break;
            }
        }
    }

    std::array<uint32_t, kMaxBatchEntries> applied{};
    for (const std::vector<SetEntry> &list : lists) {
        for (size_t first = 0; first < list.size(); first += kMaxBatchEntries) {
            const size_t count = std::min(kMaxBatchEntries, list.size() - first);
            const Status st = m_params.set(std::span<const SetEntry>(list).subspan(first, count), applied);
            if (!st.ok()) {
                return st;
            }
        }
    }
    if (!m_pending.active()) {
        m_params.setNvsDirty(false);
    }
    return Status::success();
}

Status Core::factoryReset()
{
    if (m_store == nullptr) {
        return Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    if (m_params.armed()) {
        return Status::fail(Result::Armed, Detail::None);
    }
    const Status st = persist::eraseAll(*m_store);
    if (st.ok()) {
        m_newest_gen = 0;
        m_complete_gen = 0;
    }
    return st;
}

Status Core::exitSafeMode()
{
    core_os::storeCrashCount(0);
    return Status::success();
}

// -- state -----------------------------------------------------------------------

size_t Core::instanceCount() const
{
    size_t n = 0;
    for (size_t i = 1; i < m_table.size(); ++i) {
        n += (m_table.at(i).state == InstanceState::Running) ? 1U : 0U;
    }
    return n;
}

size_t Core::instancesFailed() const
{
    size_t n = m_left_out;
    for (size_t i = 1; i < m_table.size(); ++i) {
        const InstanceState s = m_table.at(i).state;
        n += (s == InstanceState::Failed || s == InstanceState::Invalid || s == InstanceState::DependencyMissing) ? 1U
                                                                                                                    : 0U;
    }
    return n;
}

uint32_t Core::loopStackHighWater(uint8_t id) const
{
    const size_t slot = loopSlot(id);
    return slot < m_config.max_loops ? m_tasks[slot].stackHighWaterMark() : 0U;
}

bool Core::applySystemParam(void *ctx, const Ref &ref, uint32_t bits)
{
    auto *self = static_cast<Core *>(ctx);
    if (ref.type == defs::kSystemTypeId && ref.id == sys::kWatchdogMs) {
        self->m_watchdog_us.store(bits * 1000U, std::memory_order_relaxed); return true;
    }
    if (ref.type != defs::kLoopTypeId || ref.id != defs::loop::kRateHz) { return true; }
    const size_t slot = self->loopSlot(ref.instance);
    if (slot == self->m_config.max_loops) { return true; } // offline configuration
    const uint32_t period = periodUsFromHz(bits);
    const auto err = self->m_tasks[slot].setPeriod(period);
    if (err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kTimerRestartFailed, err, ref.instance, period);
        return false;
    }
    self->m_loops[slot]->setPeriodUs(period);
    return true;
}

Status Core::runAction(Ref ref, const ActionArgs &args, ActionOut &out)
{
    auto *record = m_table.find(ref.type, ref.instance);
    if (!record || !record->object) { return Status::fail(Result::NotFound, Detail::UnknownRef); }
    const auto *item = defs::findItem(*record->desc, ref.id);
    if (!item) { return Status::fail(Result::NotFound, Detail::UnknownRef); }
    if (item->kind != defs::ItemKind::Action) { return Status::fail(Result::InvalidArgument, Detail::WrongKind); }
    if (item->runs_in == defs::ActionPlacement::Paused && findLoop(record->loop) && findLoop(record->loop)->realtime() && m_params.armed()) {
        return Status::fail(Result::Armed, Detail::LockedWhileArmed);
    }
    if (args.blob.size() > kActionBlobBytes ||
        (!args.blob.empty() && item->blob != defs::BlobDir::In && item->blob != defs::BlobDir::Both)) {
        return Status::fail(Result::InvalidArgument, Detail::BadValue);
    }
    for (size_t i = 0; i < args.values.size(); ++i) {
        if (i >= item->args.size()) {
            if (args.values[i]) { return Status::fail(Result::InvalidArgument, Detail::BadValue); }
            continue;
        }
        defs::ItemDesc arg{}; arg.type = item->args[i].type;
        const auto check = checkValue(arg,args.values[i]);
        if (!check.ok) { return Status::fail(Result::InvalidArgument, check.detail, static_cast<uint16_t>(i)); }
    }
    if (!m_running || !findLoop(record->loop)) { return Status::fail(Result::Busy, Detail::NotAtBoot); }
    auto &slot = loop(record->loop).actionSlot();
    if (slot.state.load(std::memory_order_acquire) != ActionState::Empty) {
        return Status::fail(Result::Busy, Detail::StagingBusy);
    }
    slot.completed.clear(); slot.target = record->object.get(); slot.id = ref.id;
    slot.paused = item->runs_in == defs::ActionPlacement::Paused;
    slot.values = args.values; slot.blob_size = args.blob.size(); slot.out = {};
    std::copy(args.blob.begin(),args.blob.end(),slot.blob.begin());
    slot.state.store(ActionState::Ready,std::memory_order_release);
    const auto deadline=core_os::nowUs()+ParamService::stagingTimeoutUs(loop(record->loop).periodUs());
    while(slot.state.load(std::memory_order_acquire)==ActionState::Ready) {
        const auto remaining=deadline-core_os::nowUs();
        if(remaining<=0) {
            ActionState expected=ActionState::Ready;
            if(slot.state.compare_exchange_strong(expected,ActionState::Empty,std::memory_order_acq_rel)) {
                return Status::fail(Result::Busy,Detail::Withdrawn);
            }
            break;
        }
        slot.completed.wait(static_cast<uint32_t>(remaining));
    }
    // Once claimed, the action cannot be reported as unapplied. Bounded loop
    // actions finish before signalling; paused actions run here after the ack.
    while (slot.state.load(std::memory_order_acquire) == ActionState::Running) { slot.completed.wait(1000); }
    if (slot.state.load(std::memory_order_acquire) == ActionState::Paused) {
        slot.status = record->object->runAction(ref.id,args,slot.out);
    }
    Status status = slot.status;
    if (slot.out.blob_size > kActionBlobBytes ||
        (slot.out.blob_size && item->blob != defs::BlobDir::Out && item->blob != defs::BlobDir::Both)) {
        status = Status::fail(Result::Fault,Detail::BadValue);
    }
    out = slot.out;
    slot.state.store(ActionState::Empty,std::memory_order_release);
    return status;
}

} // namespace core
