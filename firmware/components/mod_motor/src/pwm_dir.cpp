#include "mod_motor/modules.hpp"
#include "mod_motor/interfaces.hpp"
#include "mod_motor/hardware.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/type_actuator_pwm_dir.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace mod_motor {
namespace {
namespace defs = drivelink::protocol::defs;
namespace d = defs::actuator_pwm_dir;
namespace codes = defs::diag::codes;
// actuator_pwm_dir.InitFailed arg0 stages (documented in diag_codes.yaml).
constexpr uint32_t kStartStagePins = 1;
constexpr uint32_t kStartStageDriver = 2;

/// actuator.pwm_dir: a controller-owned PWM+DIR output.
///
/// The output is forced low in hardware from creation until an ARM
/// transition releases it. `m_stopped` mirrors the force and gates the duty.
/// emergencyStop() touches only the driver's force and atomics, so it is safe
/// concurrently with tick() and onArmTransition(). Every stop first bumps
/// `m_stop_epoch`; a release that a stop raced (its force landed before the
/// release, or its gate store before the loop's) sees the epoch move and
/// stops again, so a monitor stop is never undone by a stale release.
class PwmDir final : public Actuator {
public:
    bool start(core::InstanceContext &ctx) override {
        m_instance = ctx.instanceId();
        const bool bound = ctx.bindParam(d::kPwmPin, &m_pwm_pin) && ctx.bindParam(d::kDirPin, &m_dir_pin) &&
            ctx.bindParam(d::kInvert, &m_invert) && ctx.bindParam(d::kDutyLimit, &m_limit) &&
            ctx.bindParam(d::kPwmFreqHz, &m_pwm_freq_hz) &&
            ctx.bindSignal(d::kAppliedDuty, &m_applied) && ctx.bindSignal(d::kForced, &m_forced);
        if (!bound) { return false; }
        if (m_pwm_pin == defs::kNoGpio || m_dir_pin == defs::kNoGpio) {
            ctx.fail(kStartStagePins, -1);
            return false;
        }
        // The hardware is taken last: an instance that failed after taking it
        // would hold an output that the monitor's actuator list never stops.
        core_os::Err err = core_os::kOk;
        m_out = hw::openPwmDir(m_pwm_pin, m_dir_pin, m_pwm_freq_hz, err);
        if (!m_out) {
            ctx.fail(kStartStageDriver, err != core_os::kOk ? err : -1);
            return false;
        }
        return true;
    }

    void tick(const core::TickContext &) override {
        float duty = (!m_stopped.load(std::memory_order_acquire) && m_port->claimed) ? m_port->duty : 0.F;
        if (!std::isfinite(duty)) { duty = 0; }
        duty = std::clamp(duty, -m_limit, m_limit);
        // A nonzero duty is written only when the previous request had the
        // same sign or was zero, so the previous write was zero or of the same
        // sign. The compare value takes effect at the next PWM period but DIR
        // at once (and a zero write leaves DIR), so DIR never flips under a
        // live compare value of the other sign: pwm_freq_hz keeps the PWM
        // period below the shortest loop period. A reversal costs one zero
        // tick; a request alternating sign every tick writes zero.
        const float requested = duty;
        if ((duty > 0 && m_prev_request < 0) || (duty < 0 && m_prev_request > 0)) { duty = 0; }
        m_prev_request = requested;
        const bool written = m_out->write(m_invert ? -duty : duty) == core_os::kOk;
        if (!written) {
            stop();
            duty = 0;
            m_prev_request = 0;
        }
        updateFault(!written || m_force_failed.load(std::memory_order_seq_cst));
        m_applied = duty;
        m_forced = m_stopped.load(std::memory_order_acquire);
    }

    void onArmTransition(bool armed) override {
        m_prev_request = 0;
        m_applied = 0;
        if (!armed) {
            stop();
            m_forced = true;
            return;
        }
        // Zero duty first, then release the hardware, then open the gate.
        const uint32_t epoch = m_stop_epoch.load(std::memory_order_seq_cst);
        if (m_out->write(0) != core_os::kOk || m_out->releaseForce() != core_os::kOk) {
            stop();
            raiseFault();
            return;
        }
        m_stopped.store(false, std::memory_order_seq_cst);
        // A stop that began after the epoch read may have forced before the
        // release landed, or closed the gate before the store above: redo it.
        if (m_stop_epoch.load(std::memory_order_seq_cst) != epoch) { stop(); }
        m_forced = m_stopped.load(std::memory_order_seq_cst);
    }

    void emergencyStop() override {
        if (m_out) { stop(); }
    }

    core::Status runAction(uint16_t, const core::ActionArgs &, core::ActionOut &) override {
        return core::Status::fail(core::Result::InvalidArgument, core::Detail::UnknownRef);
    }

private:
    /// Force low and close the gate. Runs on the loop or on the monitor.
    void stop() {
        m_stop_epoch.fetch_add(1, std::memory_order_seq_cst);
        const core_os::Err err = m_out->forceLow();
        m_stopped.store(true, std::memory_order_seq_cst);
        m_force_failed.store(err != core_os::kOk, std::memory_order_seq_cst);
        // Real-time safe and idempotent: a failed force disarms at once.
        if (err != core_os::kOk) { raiseFault(); }
    }
    /// Either side. The flag is set before the raise, so a concurrent clear
    /// by the loop either precedes the raise or sees the flag.
    void raiseFault() {
        m_fault_raised.store(true, std::memory_order_seq_cst);
        core_os::raiseDiag(codes::actuator_pwm_dir::kDriverFault, m_instance);
    }
    /// Loop side: DriverFault is active from the first faulty tick until a
    /// tick whose write and last force succeeded, whoever raised it.
    void updateFault(bool faulty) {
        if (faulty) {
            if (!m_fault_raised.load(std::memory_order_seq_cst)) { raiseFault(); }
        } else if (m_fault_raised.exchange(false, std::memory_order_seq_cst)) {
            core_os::clearDiag(codes::actuator_pwm_dir::kDriverFault, m_instance);
            // A monitor force that failed during this clear raised either
            // before it (re-raise here) or after it (still active).
            if (m_force_failed.load(std::memory_order_seq_cst)) { raiseFault(); }
        }
    }

    std::unique_ptr<hw::PwmDirOutput> m_out;  // set in start(), immutable after
    std::atomic<bool> m_stopped{true};         // the gate: loop and monitor
    std::atomic<bool> m_force_failed{false};   // the last force failed
    std::atomic<bool> m_fault_raised{false};   // DriverFault raised, not yet cleared
    std::atomic<uint32_t> m_stop_epoch{0};     // bumped by every stop
    uint8_t m_instance = 255;
    uint8_t m_pwm_pin = defs::kNoGpio, m_dir_pin = defs::kNoGpio;
    uint32_t m_pwm_freq_hz = 0;
    bool m_invert = false;
    bool m_forced = true;                      // signal; loop-owned
    float m_limit = 1, m_applied = 0, m_prev_request = 0;
};

core::Instance *make(const core::InstanceConfig &) { return new PwmDir; }
} // namespace

const core::ModuleType kPwmDirType{d::kDesc, make};
} // namespace mod_motor
