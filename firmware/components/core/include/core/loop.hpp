#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/instance.hpp"
#include "core_os/event.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core {

inline constexpr uint8_t kFactoryControlLoop = 0;
inline constexpr uint8_t kFactorySensorLoop = 1;
inline constexpr size_t kMaxLoops = 8;

/// The most params one batch can stage.
inline constexpr size_t kMaxBatchEntries = 64;

/// The staging slot's state. The config task moves it Empty → Filling →
/// Ready (or back Ready → Empty to withdraw) and Applied → Empty; the loop
/// moves it Ready → Applying → Applied. Every move is an atomic store or
/// compare-and-swap, so neither side ever waits for the other.
enum class StagingState : uint8_t {
    Empty,
    Filling,
    Ready,
    Applying,
    Applied,
};

/// One staged param value.
struct StagedEntry {
    void *ptr;          ///< the bound storage
    uint32_t bits;      ///< the value to copy in (already checked and clamped)
    OnChange hook;      ///< nullptr when the param has none
    void *hook_ctx;     ///< the instance
    uint32_t *shadow;   ///< the config task's copy, updated after Applied
    defs::ValueType type;
    uint16_t id;
};

/// Loop statistics, written by the loop and read by anyone.
struct LoopStats {
    uint32_t ticks;       ///< tick runs
    uint32_t overruns;    ///< periods skipped: the sum of (elapsed - 1)
    uint32_t last_run_us; ///< duration of the last tick
    uint32_t max_run_us;  ///< longest tick so far
};

/// A loop: a fixed list of instances, ticked in order by a `PeriodicTask`
/// that the `Core` owns (tests call `runTick` directly), plus one staging
/// slot for param batches.
///
/// The instance list has a fixed capacity, set at construction, and is
/// filled at boot only (before the task starts).
class Loop {
public:
    Loop(uint8_t id, const char *name, bool realtime, size_t capacity, uint32_t period_us,
         LoopStats *published = nullptr);

    Loop(const Loop &) = delete;
    Loop &operator=(const Loop &) = delete;

    uint8_t id() const { return m_id; }
    const char *name() const { return m_name; }
    bool realtime() const { return m_realtime; }
    void enable(bool enabled) { m_enabled.store(enabled, std::memory_order_release); }

    /// Append an instance. Boot only: the core adds them in tick order.
    /// @return false when the list is full.
    bool addInstance(Instance *instance);
    size_t instanceCount() const { return m_count; }

    /// One tick, following the plan's tick order:
    ///   1. increment the heartbeat
    ///   3. swap in a staged batch (Ready → Applying, copy, hooks, Applied,
    ///      signal the config task)
    ///   7. tick() each instance in list order (the wiring's tick order, fixed at
///      boot)
    ///   9. update the statistics
    /// Real-time: no lock, no logging, no allocation, no blocking, and the
    /// work is bounded (kMaxBatchEntries copies plus the instances).
    void runTick(uint32_t elapsed);

    /// `core_os::TickFn` for the loop's PeriodicTask: `ctx` is the Loop.
    static void taskEntry(void *ctx, uint32_t elapsed);

    uint32_t heartbeat() const { return m_heartbeat.load(std::memory_order_relaxed); }
    uint32_t periodUs() const { return m_period_us.load(std::memory_order_relaxed); }
    void setPeriodUs(uint32_t period_us) { m_period_us.store(period_us, std::memory_order_relaxed); }
    LoopStats stats() const;

    // -- staging slot (config task side) ---------------------------------------

    /// Claim the empty slot (Empty → Filling). @return false if it is in use.
    bool beginStaging();
    /// The slot's entries, to fill while Filling.
    std::span<StagedEntry, kMaxBatchEntries> stagingEntries() { return m_staging; }
    /// Publish `count` filled entries (Filling → Ready). Clears the applied
    /// event first.
    void publishStaging(size_t count);
    /// Take a Ready batch back before the loop sees it (Ready → Empty).
    /// @return false if the loop already took it.
    bool withdrawStaging();
    /// After Applied: the applied entries, for the shadow update.
    std::span<const StagedEntry> appliedEntries() const { return {m_staging.data(), m_staged_count}; }
    /// Free the slot after Applied (Applied → Empty).
    void releaseStaging();
    StagingState stagingState() const { return m_state.load(std::memory_order_acquire); }
    /// Signalled by the loop after it applied a batch.
    core_os::Event &appliedEvent() { return m_applied; }
    ActionSlot &actionSlot() { return m_action; }

private:
    void applyStaged();

    const uint8_t m_id;
    const char *const m_name;
    const bool m_realtime;
    std::atomic<bool> m_enabled{false};
    LoopStats *m_published;

    std::unique_ptr<Instance *[]> m_instances;
    const size_t m_capacity;
    size_t m_count = 0;

    std::array<StagedEntry, kMaxBatchEntries> m_staging{};
    size_t m_staged_count = 0;
    std::atomic<StagingState> m_state{StagingState::Empty};
    core_os::Event m_applied;
    ActionSlot m_action;

    std::atomic<uint32_t> m_heartbeat{0};
    std::atomic<uint32_t> m_period_us;
    std::atomic<uint32_t> m_ticks{0};
    std::atomic<uint32_t> m_overruns{0};
    std::atomic<uint32_t> m_last_run_us{0};
    std::atomic<uint32_t> m_max_run_us{0};
};

} // namespace core
