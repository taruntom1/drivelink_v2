#pragma once

#include <atomic>
#include <cstdint>

#include "core/link_settings.hpp"
#include "core/status.hpp"
#include "core_os/kv_store.hpp"

namespace core {

/// How long a link trial lasts.
struct LinkTrialTimes {
    uint32_t baud_ms = 30000;  ///< the rate changed, the pins did not
    uint32_t pins_ms = 120000; ///< a pin changed: someone moves wires
};

/// The running link, its trial and its records. Config task only, but for
/// `trialActive()`.
class LinkService {
public:
    /// What `service()` found.
    enum class Event : uint8_t {
        None,
        Expired, ///< the trial just ended unconfirmed (once): disarm and report
        Reboot,  ///< the report had its time: reboot
    };

    /// Once, by `Core::boot()`. `store` must outlive this.
    void initialize(core_os::KvStore &store, const LinkBoot &boot, const LinkTrialTimes &times, int64_t now_us);

    const LinkSettings &running() const { return m_boot.running; }
    const LinkSettings &confirmed() const { return m_boot.records.confirmed; }
    /// The link of the next boot if nothing is saved.
    LinkSettings next() const { return m_boot.next(); }
    /// A trial runs, confirmed by nobody yet. Any task.
    bool trialActive() const { return m_trial.load(std::memory_order_acquire); }
    /// A trial is saved and waits for a boot.
    bool saved() const { return m_boot.records.state == linkstore::TrialState::Armed; }

    /// What is left of the trial, rounded up: 0 only without a trial.
    uint32_t trialRemainingMs(int64_t now_us) const;

    /// Store the trial as confirmed. `InvalidArgument` / `NoLinkTrial` without
    /// one or after its time, `Fault` / `StorageFailed` when the write failed
    /// (the trial goes on).
    Status confirm(int64_t now_us);

    /// SAVE: make `wanted` the link of the next boot. The confirmed link drops
    /// a saved trial, anything else becomes one. Does nothing during a trial.
    Status save(const LinkSettings &wanted);

    /// FACTORY_RESET: drop a saved trial. The confirmed link stays.
    Status dropSaved();

    Event service(int64_t now_us);

private:
    Status closeTrialRecord();

    core_os::KvStore *m_store = nullptr;
    LinkBoot m_boot;
    std::atomic<bool> m_trial{false};
    bool m_expired = false;
    int64_t m_deadline_us = 0;
    int64_t m_reboot_us = 0;
};

} // namespace core
