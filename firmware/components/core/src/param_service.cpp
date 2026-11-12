#include "core/param_service.hpp"

#include "core/value.hpp"
#include "core_os/clock.hpp"

namespace core {

namespace {

/// How long `complete()` waits per round once the loop has taken a batch.
/// The swap itself is bounded and short; this only paces the re-check.
constexpr uint32_t kApplyingPollUs = 1000;

bool sameRef(const Ref &a, const Ref &b)
{
    return a.type == b.type && a.instance == b.instance && a.id == b.id;
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
    }
    return t;
}

Status ParamService::validate(const SetEntry &entry, uint16_t i, uint32_t &applied) const
{
    const Target t = lookup(entry.ref);
    if (t.record == nullptr || t.index == detail::kNoItem) {
        return Status::fail(Result::InvalidArgument, Detail::UnknownRef, i);
    }
    const defs::ItemDesc &item = t.record->desc->items[t.index];
    if (item.kind != defs::ItemKind::Param) {
        return Status::fail(Result::InvalidArgument, Detail::WrongKind, i);
    }
    if (defs::hasFlag(item, defs::ItemFlag::ReadOnly)) {
        return Status::fail(Result::InvalidArgument, Detail::ReadOnly, i);
    }
    if (defs::hasFlag(item, defs::ItemFlag::RebootRequired)) {
        // F3a-2 stages these into the pending layout.
        return Status::fail(Result::PendingReboot, Detail::Unsupported, i);
    }
    if (defs::hasFlag(item, defs::ItemFlag::LockedWhileArmed) && armed()) {
        return Status::fail(Result::Armed, Detail::LockedWhileArmed, i);
    }
    const ValueCheck check = checkValue(item, entry.bits);
    if (!check.ok) {
        return Status::fail(Result::InvalidArgument, check.detail, i);
    }
    if (!t.record->bindings[t.index].bound) {
        // Every live param is bound by start(); an unbound one never started.
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
    if (entries.size() > kMaxBatchEntries) {
        return Status::fail(Result::InvalidArgument, Detail::BatchTooLarge, static_cast<uint16_t>(kMaxBatchEntries));
    }
    if (applied.size() < entries.size()) {
        return Status::fail(Result::InvalidArgument, Detail::BufferTooSmall);
    }
    if (entries.empty()) {
        return Status::success();
    }

    // 1. Every entry, before anything is applied.
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto index = static_cast<uint16_t>(i);
        const Status st = validate(entries[i], index, applied[i]);
        if (!st.ok()) {
            return st;
        }
        for (size_t j = 0; j < i; ++j) {
            if (sameRef(entries[j].ref, entries[i].ref)) {
                return Status::fail(Result::InvalidArgument, Detail::DuplicateRef, index);
            }
        }
    }

    // 2. Domain: all system, or all one loop.
    const uint8_t domain = lookup(entries[0].ref).record->loop;
    for (size_t i = 1; i < entries.size(); ++i) {
        if (lookup(entries[i].ref).record->loop != domain) {
            return Status::fail(Result::InvalidArgument, Detail::CrossLoopBatch, static_cast<uint16_t>(i));
        }
    }

    if (domain == kNoLoop) {
        // 4. System: on the caller.
        return applySystem(entries, applied);
    }
    if (domain >= kLoopCount || m_loops[domain] == nullptr) {
        return Status::fail(Result::Fault, Detail::UnknownRef);
    }

    // 3. Loop: claim the slot, fill it, publish it.
    Loop &loop = *m_loops[domain];
    if (!loop.beginStaging()) {
        return Status::fail(Result::Busy, Detail::StagingBusy);
    }
    std::span<StagedEntry, kMaxBatchEntries> slot = loop.stagingEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const Target t = lookup(entries[i].ref);
        const defs::ItemDesc &item = t.record->desc->items[t.index];
        const detail::Binding &b = t.record->bindings[t.index];
        slot[i] = StagedEntry{
            .ptr = b.ptr,
            .bits = applied[i],
            .hook = b.hook,
            .hook_ctx = t.record->object.get(),
            .shadow = &t.record->shadow[t.index],
            .type = item.type,
            .id = item.id,
        };
    }
    loop.publishStaging(entries.size());
    pending.loop = &loop;
    return Status::success();
}

Status ParamService::complete(PendingSet &pending, uint32_t timeout_us)
{
    Loop *loop = pending.loop;
    pending.loop = nullptr;
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
        const defs::ItemDesc &item = t.record->desc->items[t.index];
        storeValue(item.type, t.record->bindings[t.index].ptr, applied[i]);
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

Status ParamService::get(std::span<const Ref> refs, std::span<uint32_t> values)
{
    if (values.size() < refs.size()) {
        return Status::fail(Result::InvalidArgument, Detail::BufferTooSmall);
    }
    core_os::LockGuard lock(m_mutex);
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto index = static_cast<uint16_t>(i);
        const Target t = lookup(refs[i]);
        if (t.record == nullptr || t.index == detail::kNoItem) {
            return Status::fail(Result::InvalidArgument, Detail::UnknownRef, index);
        }
        if (t.record->desc->items[t.index].kind != defs::ItemKind::Param) {
            return Status::fail(Result::InvalidArgument, Detail::WrongKind, index);
        }
        values[i] = t.record->shadow[t.index];
    }
    return Status::success();
}

} // namespace core
