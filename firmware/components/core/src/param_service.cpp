#include "core/param_service.hpp"

#include "core/pending_layout.hpp"
#include "core/value.hpp"
#include "core_os/clock.hpp"
#include "drivelink/protocol/gen/registry.hpp"

namespace core {

namespace {

/// How long `complete()` waits per round once the loop has taken a batch.
/// The swap itself is bounded and short; this only paces the re-check.
constexpr uint32_t kApplyingPollUs = 1000;

bool sameRef(const Ref &a, const Ref &b)
{
    return a.type == b.type && a.instance == b.instance && a.id == b.id;
}

size_t itemIndex(const defs::TypeDesc &desc, uint16_t id)
{
    for (size_t i = 0; i < desc.items.size(); ++i) {
        if (desc.items[i].id == id) {
            return i;
        }
    }
    return detail::kNoItem;
}

bool isPersist(const defs::ItemDesc &item)
{
    return defs::hasFlag(item, defs::ItemFlag::Persist);
}

} // namespace

ParamService::ParamService(detail::InstanceTable &table, std::array<Loop *, kLoopCount> loops,
                           SystemApplyFn system_apply, void *system_ctx)
    : m_table(table), m_loops(loops), m_system_apply(system_apply), m_system_ctx(system_ctx)
{
}

ParamService::Target ParamService::lookup(const Ref &ref) const
{
    Target t;
    t.record = m_table.find(ref.type, ref.instance);
    if (t.record != nullptr) {
        t.index = t.record->itemIndex(ref.id);
        if (t.index == detail::kNoItem) {
            return t;
        }
        t.item = &t.record->desc->items[t.index];
        t.system = (t.record->type_id == drivelink::protocol::defs::kSystemTypeId);
        if (defs::hasFlag(*t.item, defs::ItemFlag::RebootRequired)) {
            t.domain = ParamDomain::Pending;
        } else if (t.system) {
            t.domain = ParamDomain::System;
        } else if (t.record->state != InstanceState::Running) {
            t.domain = ParamDomain::Offline;
        } else {
            t.domain = (t.record->loop == kSensorLoop) ? ParamDomain::Sensor : ParamDomain::Control;
        }
        return t;
    }
    const PendingEntry *added = (m_pending != nullptr) ? m_pending->findAdded(ref.type, ref.instance) : nullptr;
    if (added != nullptr) {
        t.index = itemIndex(*added->desc, ref.id);
        if (t.index != detail::kNoItem) {
            t.item = &added->desc->items[t.index];
            t.domain = ParamDomain::Pending;
        }
    }
    return t;
}

Status ParamService::validate(const SetEntry &entry, uint16_t i, uint32_t &applied, Target &t) const
{
    t = lookup(entry.ref);
    if (t.item == nullptr) {
        return Status::fail(Result::InvalidArgument, Detail::UnknownRef, i);
    }
    const defs::ItemDesc &item = *t.item;
    if (item.kind != defs::ItemKind::Param) {
        return Status::fail(Result::InvalidArgument, Detail::WrongKind, i);
    }
    if (defs::hasFlag(item, defs::ItemFlag::ReadOnly)) {
        return Status::fail(Result::InvalidArgument, Detail::ReadOnly, i);
    }
    if (t.domain == ParamDomain::Pending && (t.system || m_pending == nullptr)) {
        // The system is not a layout entry: it has no pending copy.
        return Status::fail(Result::InvalidArgument, Detail::Unsupported, i);
    }
    if (defs::hasFlag(item, defs::ItemFlag::LockedWhileArmed) && armed()) {
        return Status::fail(Result::Armed, Detail::LockedWhileArmed, i);
    }
    const ValueCheck check = checkValue(item, entry.bits);
    if (!check.ok) {
        return Status::fail(Result::InvalidArgument, check.detail, i);
    }
    if ((t.domain == ParamDomain::Control || t.domain == ParamDomain::Sensor || t.domain == ParamDomain::System) &&
        !t.record->bindings[t.index].bound) {
        // Every live param of a running instance is bound by start().
        return Status::fail(Result::Fault, Detail::StartFailed, i);
    }
    applied = check.applied_bits;
    return Status::success();
}

Status ParamService::set(std::span<const SetEntry> entries, std::span<uint32_t> applied)
{
    PendingSet pending;
    const Status st = stage(entries, applied, pending);
    if (!st.ok() || !pending.active()) {
        return st;
    }
    return complete(pending, stagingTimeoutUs(pending.loop->periodUs()));
}

Status ParamService::stage(std::span<const SetEntry> entries, std::span<uint32_t> applied, PendingSet &pending)
{
    pending.loop = nullptr;
    pending.persist = false;
    if (entries.size() > kMaxBatchEntries) {
        return Status::fail(Result::InvalidArgument, Detail::BatchTooLarge, static_cast<uint16_t>(kMaxBatchEntries));
    }
    if (applied.size() < entries.size()) {
        return Status::fail(Result::InvalidArgument, Detail::BufferTooSmall);
    }
    if (entries.empty()) {
        return Status::success();
    }

    // 1. Every entry, before anything is applied. 2. One domain.
    ParamDomain domain = ParamDomain::System;
    bool persist = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto index = static_cast<uint16_t>(i);
        Target t;
        const Status st = validate(entries[i], index, applied[i], t);
        if (!st.ok()) {
            return st;
        }
        for (size_t j = 0; j < i; ++j) {
            if (sameRef(entries[j].ref, entries[i].ref)) {
                return Status::fail(Result::InvalidArgument, Detail::DuplicateRef, index);
            }
        }
        if (i == 0) {
            domain = t.domain;
        } else if (t.domain != domain) {
            return Status::fail(Result::InvalidArgument, Detail::CrossLoopBatch, index);
        }
        persist = persist || isPersist(*t.item);
    }

    switch (domain) {
    case ParamDomain::Pending: {
        const Status st = m_pending->setValues(entries, applied);
        if (!st.ok()) {
            return st;
        }
        setNvsDirty(true);
        return Status::fail(Result::PendingReboot, Detail::None);
    }
    case ParamDomain::Offline: {
        const Status st = applyOffline(entries, applied);
        if (st.ok() && persist) {
            setNvsDirty(true);
        }
        return st;
    }
    case ParamDomain::System: {
        const Status st = applySystem(entries, applied);
        if (st.ok() && persist) {
            setNvsDirty(true);
        }
        return st;
    }
    case ParamDomain::Control:
    case ParamDomain::Sensor:
        break;
    }

    // 3. Loop: claim the slot, fill it, publish it.
    const uint8_t loop_id = (domain == ParamDomain::Sensor) ? kSensorLoop : kControlLoop;
    if (m_loops[loop_id] == nullptr) {
        return Status::fail(Result::Fault, Detail::UnknownRef);
    }
    Loop &loop = *m_loops[loop_id];
    if (!loop.beginStaging()) {
        return Status::fail(Result::Busy, Detail::StagingBusy);
    }
    std::span<StagedEntry, kMaxBatchEntries> slot = loop.stagingEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const Target t = lookup(entries[i].ref);
        const detail::Binding &b = t.record->bindings[t.index];
        slot[i] = StagedEntry{
            .ptr = b.ptr,
            .bits = applied[i],
            .hook = b.hook,
            .hook_ctx = t.record->object.get(),
            .shadow = &t.record->shadow[t.index],
            .type = t.item->type,
            .id = t.item->id,
        };
    }
    loop.publishStaging(entries.size());
    pending.loop = &loop;
    pending.persist = persist;
    return Status::success();
}

Status ParamService::complete(PendingSet &pending, uint32_t timeout_us)
{
    Loop *loop = pending.loop;
    const bool persist = pending.persist;
    pending.loop = nullptr;
    pending.persist = false;
    if (loop == nullptr) {
        return Status::success();
    }

    const int64_t deadline = core_os::nowUs() + static_cast<int64_t>(timeout_us);
    while (loop->stagingState() != StagingState::Applied) {
        const int64_t now = core_os::nowUs();
        if (now >= deadline) {
            break;
        }
        (void)loop->appliedEvent().wait(static_cast<uint32_t>(deadline - now));
    }

    if (loop->stagingState() != StagingState::Applied) {
        if (loop->withdrawStaging()) {
            return Status::fail(Result::Busy, Detail::Withdrawn);
        }
        // The loop took it just now; its swap is bounded, so wait it out.
        while (loop->stagingState() != StagingState::Applied) {
            (void)loop->appliedEvent().wait(kApplyingPollUs);
        }
    }

    {
        core_os::LockGuard lock(m_mutex);
        for (const StagedEntry &e : loop->appliedEntries()) {
            *e.shadow = e.bits;
        }
    }
    loop->releaseStaging();
    if (persist) {
        setNvsDirty(true);
    }
    return Status::success();
}

Status ParamService::applySystem(std::span<const SetEntry> entries, std::span<const uint32_t> applied)
{
    core_os::LockGuard lock(m_mutex);

    for (size_t i = 0; i < entries.size(); ++i) {
        const uint16_t id = entries[i].ref.id;
        if (m_system_apply != nullptr && !m_system_apply(m_system_ctx, id, applied[i])) {
            // Roll back what was applied, newest first, from the shadow (not
            // updated yet).
            for (size_t j = i; j-- > 0;) {
                const Target t = lookup(entries[j].ref);
                (void)m_system_apply(m_system_ctx, entries[j].ref.id, t.record->shadow[t.index]);
            }
            return Status::fail(Result::Fault, Detail::TimerFailed, static_cast<uint16_t>(i));
        }
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const Target t = lookup(entries[i].ref);
        storeValue(t.item->type, t.record->bindings[t.index].ptr, applied[i]);
        t.record->shadow[t.index] = applied[i];
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const Target t = lookup(entries[i].ref);
        const OnChange hook = t.record->bindings[t.index].hook;
        if (hook != nullptr) {
            hook(t.record->object.get(), entries[i].ref.id);
        }
    }
    return Status::success();
}

Status ParamService::applyOffline(std::span<const SetEntry> entries, std::span<const uint32_t> applied)
{
    core_os::LockGuard lock(m_mutex);
    for (size_t i = 0; i < entries.size(); ++i) {
        const Target t = lookup(entries[i].ref);
        t.record->shadow[t.index] = applied[i];
    }
    return Status::success();
}

Status ParamService::get(std::span<const Ref> refs, std::span<uint32_t> values)
{
    if (values.size() < refs.size()) {
        return Status::fail(Result::InvalidArgument, Detail::BufferTooSmall);
    }
    core_os::LockGuard lock(m_mutex);
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto index = static_cast<uint16_t>(i);
        const detail::InstanceRecord *record = m_table.find(refs[i].type, refs[i].instance);
        const size_t item = (record != nullptr) ? record->itemIndex(refs[i].id) : detail::kNoItem;
        if (item == detail::kNoItem) {
            return Status::fail(Result::InvalidArgument, Detail::UnknownRef, index);
        }
        if (record->desc->items[item].kind != defs::ItemKind::Param) {
            return Status::fail(Result::InvalidArgument, Detail::WrongKind, index);
        }
        values[i] = record->shadow[item];
    }
    return Status::success();
}

} // namespace core
