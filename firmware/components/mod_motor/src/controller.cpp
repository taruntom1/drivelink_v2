#include "mod_motor/modules.hpp"
#include "mod_motor/interfaces.hpp"
#include "pid_kernel.hpp"
#include "core_os/clock.hpp"
#include "drivelink/protocol/gen/type_controller_cascade_pid.hpp"
#include <algorithm>

namespace mod_motor {
namespace {
namespace d = drivelink::protocol::defs::controller_cascade_pid;
enum class Mode : uint8_t { OpenLoop, Velocity, Position };
constexpr uint32_t kStartStageReferences = 1;
constexpr uint32_t kStartStageClaim = 2;
class Controller final : public core::Instance {
public:
    ~Controller() override {
        if (m_port) { m_port->duty = 0; m_port->claimed = false; }
    }
    bool start(core::InstanceContext &ctx) override {
        auto *a = ctx.ref(d::kActuator), *f = ctx.ref(d::kFeedback);
        auto *actuator = a ? static_cast<Actuator *>(a->queryInterface(kActuatorInterface)) : nullptr;
        m_feedback = f ? static_cast<Feedback *>(f->queryInterface(kFeedbackInterface)) : nullptr;
        if (!actuator || !m_feedback) { ctx.fail(kStartStageReferences, -1); return false; }
        m_port = actuator->claim();
        if (!m_port) { ctx.fail(kStartStageClaim, -1); return false; }
        bool ok = ctx.bindParam(d::kActuator, &m_actuator_ref) && ctx.bindParam(d::kFeedback, &m_feedback_ref) &&
            ctx.bindParam(d::kMode, &m_mode, &modeChanged) && ctx.bindParam(d::kDutyLimit, &m_duty_limit) &&
            ctx.bindParam(d::kVelocityLimit, &m_velocity_limit) &&
            ctx.bindSignal(d::kPosition, &m_position) && ctx.bindSignal(d::kVelocity, &m_velocity) &&
            ctx.bindSignal(d::kDuty, &m_duty) && ctx.bindSignal(d::kVelocityTarget, &m_velocity_target) &&
            ctx.bindSignal(d::kWaitingForCommand, &m_waiting) &&
            ctx.bindInput(d::kVelocitySp, &m_inputs[1], &m_receipts[1]) &&
            ctx.bindInput(d::kPositionSp, &m_inputs[2], &m_receipts[2]) &&
            ctx.bindInput(d::kDutySp, &m_inputs[0], &m_receipts[0]);
        ok = bindPid(ctx, m_vel, d::kVelKp, d::kVelError) && ok;
        ok = bindPid(ctx, m_pos, d::kPosKp, d::kPosError) && ok;
        m_requested = m_mode;
        return ok;
    }
    void tick(const core::TickContext &t) override {
        const auto &measurement = m_feedback->measurement();
        m_position = measurement.position; m_velocity = measurement.velocity;
        if (m_reset_pending) {
            m_hold[0] = 0; m_hold[1] = 0; m_hold[2] = m_position;
            m_vel.reset(); m_pos.reset(); m_duty = 0; m_velocity_target = 0;
            m_reset_pending = false;
        }
        if (!m_armed) { m_duty = m_port->duty = 0; return; }
        if (!std::isfinite(m_position) || !std::isfinite(m_velocity)) {
            // Feedback implementations must report acquisition faults via
            // diagnostics. Never send a nonfinite measurement into control.
            m_duty = m_port->duty = 0; m_vel.reset(); m_pos.reset(); return;
        }
        if (m_switch_pending) {
            m_hold[0] = std::clamp(m_duty, -m_duty_limit, m_duty_limit);
            m_hold[1] = std::clamp(m_velocity, -m_velocity_limit, m_velocity_limit);
            m_hold[2] = m_position;
            m_vel.track(m_velocity, m_velocity, m_hold[0], m_duty_limit);
            m_pos.track(m_position, m_position, m_hold[1], m_velocity_limit);
            m_velocity_target = m_hold[1];
            m_waiting = true; m_switch_pending = false;
            m_duty = m_port->duty = m_hold[0];
            return; // even a post-boundary command waits for the next tick
        }
        const auto index = static_cast<size_t>(m_mode);
        if (m_receipts[index] > m_boundary_us) { m_hold[index] = m_inputs[index]; m_waiting = false; }
        if (m_mode == Mode::OpenLoop) {
            m_duty = std::clamp(m_hold[0], -m_duty_limit, m_duty_limit);
            m_velocity_target = 0;
        } else {
            m_velocity_target = m_mode == Mode::Position ?
                m_pos.compute(m_position, m_hold[2], t.dt_s, m_velocity_limit) :
                std::clamp(m_hold[1], -m_velocity_limit, m_velocity_limit);
            m_duty = m_vel.compute(m_velocity, m_velocity_target, t.dt_s, m_duty_limit);
        }
        m_port->duty = m_duty;
    }
    void onArmTransition(bool armed) override {
        m_armed = armed; m_reset_pending = true; m_switch_pending = false;
        m_duty = m_port->duty = 0; m_boundary_us = 0; m_waiting = true;
        m_vel.reset(); m_pos.reset();
    }
    core::Status runAction(uint16_t id, const core::ActionArgs &, core::ActionOut &) override {
        if (id != d::kResetIntegrators) { return core::Status::fail(core::Result::InvalidArgument, core::Detail::UnknownRef); }
        m_vel.reset(); m_pos.reset(); return {};
    }
private:
    static bool bindPid(core::InstanceContext &ctx, pid::Kernel &p, uint16_t param, uint16_t signal) {
        return ctx.bindParam(param, &p.kp) && ctx.bindParam(param+1, &p.ki) &&
            ctx.bindParam(param+2, &p.kd) && ctx.bindParam(param+3, &p.cutoff) &&
            ctx.bindSignal(signal, &p.error) && ctx.bindSignal(signal+1, &p.p) &&
            ctx.bindSignal(signal+2, &p.i) && ctx.bindSignal(signal+3, &p.d) && ctx.bindSignal(signal+4, &p.i);
    }
    static void modeChanged(void *self, uint16_t) {
        auto &c = *static_cast<Controller *>(self);
        if (c.m_mode == c.m_requested) { return; }
        c.m_requested = c.m_mode;
        c.m_boundary_us = core_os::nowUs();
        c.m_switch_pending = c.m_armed; c.m_waiting = true;
    }
    std::shared_ptr<ControlPort> m_port;
    Feedback *m_feedback = nullptr;
    pid::Kernel m_vel, m_pos;
    uint32_t m_actuator_ref = 0, m_feedback_ref = 0;
    Mode m_mode = Mode::OpenLoop, m_requested = Mode::OpenLoop;
    float m_duty_limit = 1, m_velocity_limit = 100;
    float m_position = 0, m_velocity = 0, m_duty = 0, m_velocity_target = 0;
    std::array<float, 3> m_inputs{}, m_hold{};
    std::array<int64_t, 3> m_receipts{};
    int64_t m_boundary_us = 0;
    bool m_armed = false, m_reset_pending = true, m_switch_pending = false, m_waiting = true;
};
core::Instance *make(const core::InstanceConfig &) { return new Controller; }
const core::ModuleType controller{d::kDesc, make};
}
const std::array<const core::ModuleType *, 1> kTypes{&controller};
} // namespace mod_motor
