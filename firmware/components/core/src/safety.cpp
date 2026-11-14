#include "core/core.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

namespace core {
namespace codes = drivelink::protocol::defs::diag::codes::core;
Status Core::arm() {
    const auto ticket = m_arm.ticket();
    if (ticket & ArmState::kFault) { return Status::fail(Result::Fault, Detail::FaultLatched); }
    if (ticket & ArmState::kStopping) { return Status::fail(Result::Busy, Detail::DisarmRaced); }
    if (!m_running) { return Status::fail(Result::Busy, Detail::NotAtBoot); }
    if (m_safe_mode) { return Status::fail(Result::Fault, Detail::SafeMode); }
    if (m_layout_invalid) { return Status::fail(Result::Fault, Detail::LayoutInvalid); }
    if (pendingReboot()) { return Status::fail(Result::Fault, Detail::RebootPending); }
    if (m_ota.blocksArm()) { return Status::fail(Result::Busy,m_ota.pending() ? Detail::OtaPendingConfirm : Detail::OtaActive); }
    if (instancesFailed()) { return Status::fail(Result::Fault, Detail::StartFailed); }
    if (core_os::diagBlocksArm()) { return Status::fail(Result::Fault, Detail::BlocksArm); }
    bool configured = false;
    for (size_t i = 0; i < m_groups.capacity(GroupKind::Command); ++i) {
        configured |= m_groups.info(GroupKind::Command, static_cast<uint8_t>(i))->configured;
    }
    if (!configured) { return Status::fail(Result::InvalidArgument, Detail::NoCommandGroup); }
    if (ticket & ArmState::kArmed) {
        return ticket == m_arm.ticket() ? Status::success() : Status::fail(Result::Busy, Detail::DisarmRaced);
    }
    return m_arm.commit(ticket, core_os::nowUs()) ? Status::success() : Status::fail(Result::Busy, Detail::DisarmRaced);
}

Status Core::acknowledgeFault() {
    const auto ticket = m_arm.ticket();
    if (ticket & ArmState::kArmed) { return Status::fail(Result::Armed, Detail::LockedWhileArmed); }
    if (core_os::diagDisarmsActive()) { return Status::fail(Result::Fault, Detail::FaultActive); }
    return m_arm.acknowledge(ticket) ? Status::success() : Status::fail(Result::Busy, Detail::DisarmRaced);
}

void Core::monitor(int64_t now) {
    uint32_t word = 0; int64_t armed_at = 0;
    if (!m_arm.snapshot(word, armed_at)) { return; }
    const bool armed = (word & ArmState::kArmed) && !(word & (ArmState::kFault | ArmState::kStopping));
    bool stalled = false, stop_claimed = false;
    for (size_t i = 0; i < m_config.max_loops; ++i) {
        auto *loop = m_loops[i]; if (!loop) { continue; }
        auto &hb = m_heartbeats[i];
        const auto value = loop->heartbeat();
        if (value != hb.value) {
            hb.value = value; hb.advanced_us = now;
            if (hb.stalled) { core_os::clearDiag(codes::kLoopStalled, loop->id()); hb.stalled = false; }
        }
        if (armed && now - hb.advanced_us > static_cast<int64_t>(3U * loop->periodUs() + 5000U)) {
            // Publish the stopping gate before raising: a re-arm cannot
            // release the force while this callback is applying it.
            if (!stop_claimed) {
                if (!m_arm.claimStop(word)) { return; } // a disarm/re-arm invalidated this observation
                stop_claimed = true;
            }
            core_os::raiseDiag(codes::kLoopStalled, loop->id());
            hb.stalled = true; stalled = true;
        }
    }
    bool expired = false;
    if (armed && !stalled) {
        int64_t latest = armed_at; bool available = true;
        for (size_t i = 0; i < m_groups.capacity(GroupKind::Command); ++i) {
            int64_t received = 0;
            if (!m_groups.lastCommandUs(static_cast<uint8_t>(i), received)) { available = false; continue; }
            if (received > latest) { latest = received; }
        }
        // A contended nonblocking read is unknown, never proof of timeout.
        expired = available && now - latest >= m_watchdog_us.load(std::memory_order_relaxed);
        if (expired) {
            if (!m_arm.claimStop(word)) { return; }
            stop_claimed = true;
            core_os::raiseDiag(codes::kWatchdogTimeout);
            core_os::clearDiag(codes::kWatchdogTimeout);
        }
    }
    const auto after = m_arm.ticket();
    if (!stop_claimed && !(after & ArmState::kArmed)) {
        // Repeat while disarmed: even a stale transition hook on another
        // core cannot leave a released force after that loop stops running.
        stop_claimed = m_arm.claimStop(after);
    }
    if (stop_claimed) {
        for (size_t i = 0; i < m_actuator_count; ++i) { m_actuators[i]->emergencyStop(); }
        m_arm.word.fetch_and(~ArmState::kStopping, std::memory_order_release);
    }
}
} // namespace core
