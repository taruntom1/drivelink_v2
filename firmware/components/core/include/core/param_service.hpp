#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/instance_table.hpp"
#include "core/loop.hpp"
#include "core/status.hpp"
#include "core_os/mutex.hpp"

namespace core {

class PendingLayout;

/// An item address: `type u8 | instance u8 | id u16`. System items are type
/// 0x00, instance 0.
struct Ref {
    uint8_t type;
    uint8_t instance;
    uint16_t id;
};

/// One entry of a param batch: where, and the raw 4-byte slot value.
struct SetEntry {
    Ref ref;
    uint32_t bits;
};

/// Apply one system param on the caller (a loop rate restarts its timer).
/// @return false on failure; the callee reports the diag code.
using SystemApplyFn = bool (*)(void *ctx, uint16_t id, uint32_t bits);

/// A loop batch that `stage()` published and `complete()` must finish.
struct PendingSet {
    Loop *loop = nullptr;
    bool persist = false; ///< a `persist` param is in it (sets nvs_dirty when applied)

    bool active() const { return loop != nullptr; }
};

/// Where a param write goes. Every entry of a batch must have the same one.
enum class ParamDomain : uint8_t {
    System,  ///< system params: applied on the caller
    Control, ///< live params of running instances in the control loop: staged
    Sensor,  ///< ... in the sensor loop
    Offline, ///< live params of an instance that isn't running: written to its shadow on the caller
    Pending, ///< `reboot_required` params, and params of an instance that exists only in the pending layout
};

/// The param service: reads and atomic batch writes, on the config task side.
///
/// It keeps a **shadow** of every param's value bits per instance, so `get()`
/// never touches storage the loops own. A batch is all-or-nothing:
///   1. Every entry is validated first: the Ref exists (in the configured
///      records, or as an instance added to the pending layout), it is a param
///      (`WrongKind`), not `read_only`, not `locked_while_armed` while armed,
///      and its value passes `checkValue` (clamped). No Ref twice. A
///      `reboot_required` system param is `Unsupported`.
///   2. Domain (`ParamDomain`): every entry has the same one, else
///      `CrossLoopBatch`.
///   3. A loop batch claims that loop's staging slot (else `Busy` /
///      `StagingBusy`), and the config task waits up to 3 loop periods plus
///      5 ms for the loop to swap it in. On a timeout it withdraws the batch
///      with a compare-and-swap (`Busy` / `Withdrawn`, nothing applied); if
///      the loop already took it, it waits for the swap to finish. Then the
///      shadow is updated and the applied (clamped) values are returned.
///   4. A system batch is applied on the caller. On a failure the entries
///      already applied are rolled back and the answer is `Fault` /
///      `TimerFailed`.
///   5. An offline batch is written to the shadows on the caller.
///   6. A pending batch goes to the pending layout, which validates the whole
///      layout; accepted, it answers `PendingReboot` (`Status::accepted()`)
///      with the applied values.
/// On failure `Status::index` names the entry.
///
/// `nvs_dirty` is set when a `persist` param is applied (any domain) or a
/// pending batch is accepted; SAVE clears it.
///
/// Thread-safe for one caller at a time per loop (the config task); `get()`
/// may run concurrently.
class ParamService {
public:
    ParamService(detail::InstanceTable &table, std::array<Loop *, kLoopCount> loops, SystemApplyFn system_apply,
                 void *system_ctx);

    ParamService(const ParamService &) = delete;
    ParamService &operator=(const ParamService &) = delete;

    /// Route pending writes here (the core does this at construction).
    /// Without it, `reboot_required` writes are `Unsupported`.
    void setPendingLayout(PendingLayout *pending) { m_pending = pending; }

    /// Apply a batch: `stage()`, then `complete()` with the loop's timeout.
    /// `applied` (at least as long as `entries`) receives the applied values;
    /// on failure its contents are unspecified.
    Status set(std::span<const SetEntry> entries, std::span<uint32_t> applied);

    /// Validate and stage a batch. A system, offline or pending batch is
    /// applied here and leaves `pending` inactive. A loop batch is published
    /// to the loop's slot and `pending` names the loop: call `complete()`
    /// next. The applied values are in `applied` already.
    Status stage(std::span<const SetEntry> entries, std::span<uint32_t> applied, PendingSet &pending);

    /// Wait up to `timeout_us` for the loop to take a staged batch, or
    /// withdraw it. Ok when `pending` is inactive. Leaves `pending` inactive.
    Status complete(PendingSet &pending, uint32_t timeout_us);

    /// Read params from the shadow (configured records only: an instance that
    /// exists only in the pending layout is read with `Core::getPending`).
    /// `values` must be at least as long as `refs`. Signals and inputs are
    /// `WrongKind`.
    Status get(std::span<const Ref> refs, std::span<uint32_t> values);

    /// How long `set()` waits for a loop: 3 periods plus 5 ms.
    static uint32_t stagingTimeoutUs(uint32_t period_us) { return (3U * period_us) + 5000U; }

    /// The arm state, for `locked_while_armed`. Nothing sets it until F4b's
    /// arm state machine, except tests.
    void setArmed(bool armed) { m_armed.store(armed, std::memory_order_relaxed); }
    bool armed() const { return m_armed.load(std::memory_order_relaxed); }

    /// Unsaved changes: a `persist` param or the pending layout changed since
    /// the last SAVE.
    bool nvsDirty() const { return m_nvs_dirty.load(std::memory_order_relaxed); }
    void setNvsDirty(bool dirty) { m_nvs_dirty.store(dirty, std::memory_order_relaxed); }

private:
    struct Target {
        detail::InstanceRecord *record = nullptr; ///< null for a pending-only instance
        const defs::ItemDesc *item = nullptr;
        size_t index = detail::kNoItem;
        ParamDomain domain = ParamDomain::System;
        bool system = false; ///< the system record
    };

    Target lookup(const Ref &ref) const;
    Status validate(const SetEntry &entry, uint16_t i, uint32_t &applied, Target &t) const;
    Status applySystem(std::span<const SetEntry> entries, std::span<const uint32_t> applied);
    Status applyOffline(std::span<const SetEntry> entries, std::span<const uint32_t> applied);

    detail::InstanceTable &m_table;
    std::array<Loop *, kLoopCount> m_loops;
    SystemApplyFn m_system_apply;
    void *m_system_ctx;
    PendingLayout *m_pending = nullptr;
    core_os::Mutex m_mutex; ///< guards the shadows
    std::atomic<bool> m_armed{false};
    std::atomic<bool> m_nvs_dirty{false};
};

} // namespace core
