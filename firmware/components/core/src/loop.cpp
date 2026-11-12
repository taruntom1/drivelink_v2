#include "core/loop.hpp"

#include "core_os/clock.hpp"

namespace core {

Loop::Loop(uint8_t id, const char *name, defs::LoopClass loop_class, size_t capacity, uint32_t period_us)
    : m_id(id),
      m_name(name),
      m_class(loop_class),
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
    static_cast<Loop *>(ctx)->runTick(elapsed);
}

void Loop::runTick(uint32_t elapsed)
{
    const int64_t start_us = core_os::nowUs();

    // 1. Heartbeat (read by the safety monitor from F4b).
    m_heartbeat.fetch_add(1U, std::memory_order_relaxed);

    // 3. Staged param batch.
    applyStaged();

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
