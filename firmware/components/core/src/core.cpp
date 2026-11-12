#include "core/core.hpp"

#include <algorithm>
#include <cstring>
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

constexpr const char *kControlTaskName = "core_ctrl";
constexpr const char *kSensorTaskName = "core_sens";
constexpr const char *kCrashClearName = "core_crash_clr";

/// The core's own storage for the system params. It runs in no loop: the
/// param service applies system params on the config task.
class SystemInstance final : public Instance {
public:
    bool start(InstanceContext &ctx) override
    {
        ctx.bindParam(sys::kControlRateHz, &m_control_rate_hz);
        ctx.bindParam(sys::kSensorRateHz, &m_sensor_rate_hz);
        ctx.bindParam(sys::kStatusRateHz, &m_status_rate_hz);
        return true;
    }

    void tick(const TickContext &t) override { (void)t; }

private:
    uint16_t m_status_rate_hz = 10;
    uint16_t m_control_rate_hz = 0;
    uint16_t m_sensor_rate_hz = 0;
};

uint32_t defaultPeriodUs(uint16_t rate_id)
{
    const defs::ItemDesc *item = defs::findItem(sys::kDesc, rate_id);
    return periodUsFromHz((item != nullptr) ? item->default_bits : 0U);
}

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

} // namespace

Core::Core(const CoreConfig &config)
    : m_config(config),
      m_table(static_cast<size_t>(config.max_instances) + 1U), // + the system instance
      m_control(kControlLoop, "control", defs::LoopClass::Control, config.max_instances,
                defaultPeriodUs(sys::kControlRateHz)),
      m_sensor(kSensorLoop, "sensor", defs::LoopClass::Sensor, config.max_instances,
               defaultPeriodUs(sys::kSensorRateHz)),
      m_params(m_table, {&m_control, &m_sensor}, &Core::applySystemParam, this),
      m_pending(m_registry, m_table, ValidationConfig{config.gpio_allowed_mask, config.max_instances})
{
    m_params.setPendingLayout(&m_pending);
    std::unique_ptr<detail::InstanceRecord> record =
        makeRecord(sys::kDesc, drivelink::protocol::defs::kSystemTypeId, 0, kNoLoop, "system", nullptr);
    record->object = std::make_unique<SystemInstance>();
    m_system_ok = startInstance(*record).ok();
    if (m_system_ok) {
        (void)m_table.add(std::move(record)); // the table has room: it was just made
    }
}

Core::~Core()
{
    stop();
    m_crash_clear.cancel();
}

Status Core::init(std::span<const ModuleType *const> types)
{
    if (!m_system_ok) {
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
        for (size_t i = 0; i < system.desc->items.size(); ++i) {
            const defs::ItemDesc &item = system.desc->items[i];
            if (item.kind == defs::ItemKind::Param && system.bindings[i].bound) {
                storeValue(item.type, system.bindings[i].ptr, system.shadow[i]);
                (void)applySystemParam(this, item.id, system.shadow[i]); // the tasks aren't running: cannot fail
            }
        }
    } else if (cfg.found_any) {
        core_os::reportDiag(diag_codes::core::kConfigLoadFailed, 0, cfg.newest_gen, cfg.cause);
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
    (void)validateLayout(entries, ValidationConfig{m_config.gpio_allowed_mask, m_config.max_instances}, status);

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

    // 5. Create, dependencies first.
    std::vector<bool> include(n, false);
    for (size_t i = 0; i < n; ++i) {
        include[i] = records[i] != nullptr && (status[i].ok() || status[i].detail == Detail::DependencyMissing);
    }
    for (const size_t i : creationOrder(entries, include)) {
        detail::InstanceRecord &record = *records[i];
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
        include[i] = records[i] != nullptr && records[i]->state == InstanceState::Running;
    }
    for (const size_t i : tickOrder(entries, include)) {
        detail::InstanceRecord &record = *records[i];
        (void)loop(record.loop).addInstance(record.object.get()); // sized for max_instances
    }

    // 7. Clear the crash counter after a while of normal running.
    (void)m_crash_clear.start(kCrashClearName, m_config.crash_clear_delay_us, &clearCrashCount, nullptr);
    return Status::success();
}

Status Core::start()
{
    if (m_running) {
        return Status::success();
    }
    m_start_stage = 0;
    m_start_err = core_os::kOk;

    core_os::Err err = m_control_task.start(kControlTaskName, m_config.control.stack_bytes, m_config.control.priority,
                                            m_config.control.core, m_control.periodUs(), &Loop::taskEntry,
                                            &m_control);
    if (err != core_os::kOk) {
        m_start_stage = kInitStageControlTask;
        m_start_err = err;
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    err = m_sensor_task.start(kSensorTaskName, m_config.sensor.stack_bytes, m_config.sensor.priority,
                              m_config.sensor.core, m_sensor.periodUs(), &Loop::taskEntry, &m_sensor);
    if (err != core_os::kOk) {
        m_control_task.stop();
        m_start_stage = kInitStageSensorTask;
        m_start_err = err;
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    m_running = true;
    m_started = true;
    return Status::success();
}

void Core::stop()
{
    m_control_task.stop();
    m_sensor_task.stop();
    m_running = false;
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

    // One list per domain: system, control, sensor, offline.
    std::array<std::vector<SetEntry>, 4> lists;
    auto addRecord = [&](const detail::InstanceRecord &r, std::span<const ParamPair> pairs) {
        std::vector<uint32_t> v(r.desc->items.size());
        persist::applyPairs(*r.desc, pairs, v.data());
        size_t list = 3;
        if (r.type_id == drivelink::protocol::defs::kSystemTypeId) {
            list = 0;
        } else if (r.state == InstanceState::Running) {
            list = (r.loop == kSensorLoop) ? 2U : 1U;
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
    return (id == kSensorLoop) ? m_sensor_task.stackHighWaterMark() : m_control_task.stackHighWaterMark();
}

bool Core::applySystemParam(void *ctx, uint16_t id, uint32_t bits)
{
    auto *self = static_cast<Core *>(ctx);
    uint8_t loop_id = 0;
    if (id == sys::kControlRateHz) {
        loop_id = kControlLoop;
    } else if (id == sys::kSensorRateHz) {
        loop_id = kSensorLoop;
    } else {
        return true; // nothing to do beyond storing it
    }
    const uint32_t period_us = periodUsFromHz(bits);
    const core_os::Err err = self->task(loop_id).setPeriod(period_us);
    if (err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kTimerRestartFailed, err, loop_id, period_us);
        return false;
    }
    self->loop(loop_id).setPeriodUs(period_us);
    return true;
}

Status Core::runAction(Ref ref, const ActionArgs &args, ActionOut &out)
{
    auto *record = m_table.find(ref.type, ref.instance);
    if (!record || !record->object) { return Status::fail(Result::NotFound, Detail::UnknownRef); }
    const auto *item = defs::findItem(*record->desc, ref.id);
    if (!item) { return Status::fail(Result::NotFound, Detail::UnknownRef); }
    if (item->kind != defs::ItemKind::Action) { return Status::fail(Result::InvalidArgument, Detail::WrongKind); }
    if (item->runs_in == defs::ActionPlacement::Paused && record->loop == kControlLoop && m_params.armed()) {
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
    if (!m_running || record->loop >= kLoopCount) { return Status::fail(Result::Busy, Detail::NotAtBoot); }
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
