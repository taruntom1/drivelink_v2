// The link trial (core/link_service.hpp). Config task; flash writes.

#include "core/link_service.hpp"

namespace core {

namespace {

/// How long the expired trial's diagnostics get to leave before the reboot.
constexpr int64_t kReportUs = 500000;

Status storageFailed()
{
    return Status::fail(Result::Fault, Detail::StorageFailed);
}

} // namespace

void LinkService::initialize(core_os::KvStore &store, const LinkBoot &boot, const LinkTrialTimes &times,
                             int64_t now_us)
{
    m_store = &store;
    m_boot = boot;
    m_expired = false;
    const uint32_t ms = boot.running.samePins(boot.records.confirmed) ? times.baud_ms : times.pins_ms;
    m_deadline_us = now_us + (static_cast<int64_t>(ms) * 1000);
    m_trial.store(boot.trial, std::memory_order_release);
}

uint32_t LinkService::trialRemainingMs(int64_t now_us) const
{
    if (!trialActive()) {
        return 0;
    }
    const int64_t left = m_deadline_us - now_us;
    return (left <= 0) ? 1U : static_cast<uint32_t>((left + 999) / 1000);
}

Status LinkService::closeTrialRecord()
{
    return linkstore::writeTrial(*m_store, m_boot.records, linkstore::TrialState::None, m_boot.records.trial) ==
                   core_os::kOk
               ? Status::success()
               : storageFailed();
}

Status LinkService::confirm(int64_t now_us)
{
    if (m_store == nullptr || !trialActive() || m_expired || now_us >= m_deadline_us) {
        return Status::fail(Result::InvalidArgument, Detail::NoLinkTrial);
    }
    if (linkstore::writeConfirmed(*m_store, m_boot.records, m_boot.running) != core_os::kOk) {
        return storageFailed();
    }
    m_trial.store(false, std::memory_order_release);
    // Committed: the next boot runs this link whether or not the record closes
    // (it then closes it).
    (void)closeTrialRecord();
    m_boot.records.state = linkstore::TrialState::None;
    return Status::success();
}

Status LinkService::save(const LinkSettings &wanted)
{
    if (trialActive()) {
        return Status::success(); // the link cannot be edited now, and its record is in use
    }
    if (wanted == m_boot.records.confirmed) {
        return dropSaved();
    }
    if (saved() && m_boot.records.trial == wanted) {
        return Status::success();
    }
    return linkstore::writeTrial(*m_store, m_boot.records, linkstore::TrialState::Armed, wanted) == core_os::kOk
               ? Status::success()
               : storageFailed();
}

Status LinkService::dropSaved()
{
    return (trialActive() || !saved()) ? Status::success() : closeTrialRecord();
}

LinkService::Event LinkService::service(int64_t now_us)
{
    if (!trialActive() || now_us < m_deadline_us) {
        return Event::None;
    }
    if (!m_expired) {
        m_expired = true;
        m_reboot_us = now_us + kReportUs;
        return Event::Expired;
    }
    return (now_us >= m_reboot_us) ? Event::Reboot : Event::None;
}

} // namespace core
