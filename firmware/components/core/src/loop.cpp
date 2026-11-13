#include "core/loop.hpp"
#include "core/group_service.hpp"

#include "core_os/clock.hpp"
#include "core_os/realtime.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

namespace core {

Loop::Loop(uint8_t id, const char *name, bool realtime, size_t capacity, uint32_t period_us, LoopStats *published)
    : m_id(id),
      m_name(name),
      m_realtime(realtime),
      m_published(published),
      m_instances(std::make_unique<Instance *[]>(capacity)),
      m_capacity(capacity),
      m_period_us(period_us)
{
}

bool Loop::addInstance(Instance *instance)
{
    if (instance == nullptr || m_count >= m_capacity) {
        return false;
    }
    m_instances[m_count++] = instance;
    return true;
}

void Loop::taskEntry(void *ctx, uint32_t elapsed)
{
    auto *loop = static_cast<Loop *>(ctx);
    if (loop->m_enabled.load(std::memory_order_acquire)) { loop->runTick(elapsed); }
}

void Loop::runTick(uint32_t elapsed)
{
    core_os::RealtimeScope guard(m_realtime);
    const int64_t start_us = core_os::nowUs();

    // 1. Heartbeat (read by the safety monitor from F4b).
    m_heartbeat.fetch_add(1U, std::memory_order_relaxed);

    if (m_action.state.load(std::memory_order_acquire) == ActionState::Paused) { return; }
    // 3. Staged param batch.
    applyStaged();

    ActionState expected = ActionState::Ready;
    if (m_action.state.compare_exchange_strong(expected, ActionState::Running, std::memory_order_acq_rel)) {
        if (m_action.paused) {
            m_action.state.store(ActionState::Paused, std::memory_order_release);
            m_action.completed.signal();
            return;
        }
        const ActionArgs args{m_action.values, {m_action.blob.data(), m_action.blob_size}};
        m_action.status = m_action.target->runAction(m_action.id, args, m_action.out);
        m_action.state.store(ActionState::Done, std::memory_order_release);
        m_action.completed.signal();
    }

    // 5. Group swaps and command mailboxes. Disarmed until F4b.
    if (m_groups) { m_groups->beginTick(m_id); }

    // 7. Instances, in the wiring's tick order (the list order, set at boot).
    const uint32_t period_us = m_period_us.load(std::memory_order_relaxed);
    const TickContext t{
        .now_us = start_us,
        .dt_s = static_cast<float>(elapsed) * static_cast<float>(period_us) * 1e-6F,
        .elapsed = elapsed,
    };
    for (size_t i = 0; i < m_count; ++i) {
        m_instances[i]->tick(t);
    }

    // 8. Tick-owned plain storage, including loop statistics from the previous
    // tick, is sampled on its owning loop before publishing this tick stats.
    if (m_groups) { m_groups->endTick(m_id, start_us); }

    // 9. Statistics.
    m_ticks.fetch_add(1U, std::memory_order_relaxed);
    if (elapsed > 1U) {
        m_overruns.fetch_add(elapsed - 1U, std::memory_order_relaxed);
    }
    const auto run_us = static_cast<uint32_t>(core_os::nowUs() - start_us);
    m_last_run_us.store(run_us, std::memory_order_relaxed);
    if (run_us > m_max_run_us.load(std::memory_order_relaxed)) {
        m_max_run_us.store(run_us, std::memory_order_relaxed); // only the loop writes it
    }
    if (m_published) { *m_published = stats(); }
    if (run_us > period_us) {
        core_os::countDiag(drivelink::protocol::defs::diag::codes::core::kLoopOverBudget, 1, m_id);
    }
}

void Loop::applyStaged()
{
    StagingState expected = StagingState::Ready;
    if (!m_state.compare_exchange_strong(expected, StagingState::Applying, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
        return;
    }
    const size_t count = m_staged_count;
    for (size_t i = 0; i < count; ++i) {
        const StagedEntry &e = m_staging[i];
        storeValue(e.type, e.ptr, e.bits);
    }
    // Hooks after every copy, so each sees the whole batch.
    for (size_t i = 0; i < count; ++i) {
        const StagedEntry &e = m_staging[i];
        if (e.hook != nullptr) {
            e.hook(e.hook_ctx, e.id);
        }
    }
    m_state.store(StagingState::Applied, std::memory_order_release);
    m_applied.signal();
}

LoopStats Loop::stats() const
{
    return LoopStats{
        .ticks = m_ticks.load(std::memory_order_relaxed),
        .overruns = m_overruns.load(std::memory_order_relaxed),
        .last_run_us = m_last_run_us.load(std::memory_order_relaxed),
        .max_run_us = m_max_run_us.load(std::memory_order_relaxed),
    };
}

bool Loop::beginStaging()
{
    StagingState expected = StagingState::Empty;
    return m_state.compare_exchange_strong(expected, StagingState::Filling, std::memory_order_acquire,
                                           std::memory_order_relaxed);
}

void Loop::publishStaging(size_t count)
{
    m_staged_count = (count <= kMaxBatchEntries) ? count : kMaxBatchEntries;
    m_applied.clear(); // drop a stale signal from an earlier batch
    m_state.store(StagingState::Ready, std::memory_order_release);
}

bool Loop::withdrawStaging()
{
    StagingState expected = StagingState::Ready;
    return m_state.compare_exchange_strong(expected, StagingState::Empty, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

void Loop::releaseStaging()
{
    StagingState expected = StagingState::Applied;
    (void)m_state.compare_exchange_strong(expected, StagingState::Empty, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
}

} // namespace core
