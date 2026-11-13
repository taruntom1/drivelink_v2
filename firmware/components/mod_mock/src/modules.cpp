#include "mod_mock/modules.hpp"
#include "core/instance.hpp"
#include "mod_motor/interfaces.hpp"
#include "mod_motor/velocity_estimator.hpp"
#include <bit>
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/type_actuator_mock.hpp"
#include "drivelink/protocol/gen/type_feedback_mock.hpp"
#include "drivelink/protocol/gen/type_imu_mock.hpp"
#include "drivelink/protocol/gen/type_fusion_mock.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>

namespace mod_mock {
namespace {
namespace defs = drivelink::protocol::defs;
constexpr uint32_t kStartStageSimulationSource = 1;
class Actuator final : public mod_motor::Actuator {
public:
    bool start(core::InstanceContext &ctx) override {
        namespace d = defs::actuator_mock;
        instance_id = ctx.instanceId();
        return ctx.bindParam(d::kTauS, &tau) && ctx.bindParam(d::kGain, &gain) &&
            ctx.bindParam(d::kDeadband, &deadband) && ctx.bindParam(d::kDutyLimit, &limit) &&
            ctx.bindParam(d::kOneTickDelay, &delay) && ctx.bindSignal(d::kAppliedDuty, &applied) &&
            ctx.bindSignal(d::kVelocity, &velocity) && ctx.bindSignal(d::kPosition, &position) &&
            ctx.bindInput(d::kDuty, &duty);
    }
    void tick(const core::TickContext &t) override {
        float requested = std::clamp(m_port->claimed ? m_port->duty : duty, -limit, limit);
        if (std::fabs(requested) < deadband) { requested = 0; }
        if (stopped.load(std::memory_order_acquire)) { requested = 0; queued = 0; }
        applied = std::clamp(delay ? queued : requested, -limit, limit);
        queued = requested;
        const float target = applied * gain;
        const float decay = std::exp(-t.dt_s/tau);
        true_position += target*t.dt_s + (velocity-target)*tau*(1.F-decay);
        velocity = target+(velocity-target)*decay;
        position = static_cast<float>(true_position);
    }
    bool plantObservation(mod_motor::PlantObservation &out) const override {
        out = {true_position, reset_generation}; return true;
    }
    core::Status runAction(uint16_t id, const core::ActionArgs &in, core::ActionOut &out) override {
        namespace d = defs::actuator_mock;
        if (id == d::kDrive) {
            if (m_port->claimed) { return core::Status::fail(core::Result::Busy, core::Detail::AlreadyBound); }
            duty = std::clamp(defs::f32FromBits(in.values[0]), -1.F, 1.F);
            out.values[0] = defs::f32Bits(velocity); out.values[1] = defs::f32Bits(position);
        } else if (id == d::kReset) {
            velocity = position = duty = queued = applied = 0; true_position = 0;
            ++reset_generation;
            out.blob_size = in.blob.size(); std::copy(in.blob.begin(),in.blob.end(),out.blob.begin());
        } else if (id == d::kDiagnostic) {
            const auto code = in.values[0] ? defs::diag::codes::actuator_mock::kFaultInjected : defs::diag::codes::actuator_mock::kArmBlocked;
            if (in.values[1]) { core_os::raiseDiag(code, instance_id); } else { core_os::clearDiag(code, instance_id); }
        } else { return core::Status::fail(core::Result::InvalidArgument,core::Detail::UnknownRef); }
        return {};
    }
    void onArmTransition(bool armed) override {
        duty = queued = applied = m_port->duty = 0;
        stopped.store(!armed, std::memory_order_release);
    }
    void emergencyStop() override { stopped.store(true, std::memory_order_release); }
private:
    std::atomic<bool> stopped{true};
    uint8_t instance_id = 255;
    float tau = .1F, gain = 10, duty = 0, deadband = .02F, limit = 1;
    float queued = 0, applied = 0, velocity = 0, position = 0;
    double true_position = 0;
    bool delay = true;
    uint32_t reset_generation = 0;
};
class Feedback final : public mod_motor::Feedback {
public:
    bool start(core::InstanceContext &ctx) override {
        namespace d = defs::feedback_mock;
        auto *provider = ctx.ref(d::kActuator);
        source = provider ? static_cast<mod_motor::Actuator *>(provider->queryInterface(mod_motor::kActuatorInterface)) : nullptr;
        mod_motor::PlantObservation observation;
        if (provider && (!source || !source->plantObservation(observation))) { ctx.fail(kStartStageSimulationSource, -1); return false; }
        return ctx.bindParam(d::kActuator, &ref) && ctx.bindParam(d::kCountsPerRev, &cpr) &&
            ctx.bindParam(d::kGearRatio, &ratio) && ctx.bindParam(d::kVelocityCutoff, &cutoff) &&
            ctx.bindParam(d::kNoiseCounts, &noise) && ctx.bindParam(d::kNoiseSeed, &seed) &&
            ctx.bindSignal(d::kCount, &m_measurement.count) &&
            ctx.bindSignal(d::kVelocity, &m_measurement.velocity) && ctx.bindSignal(d::kPosition, &m_measurement.position);
    }
    void tick(const core::TickContext &t) override {
        mod_motor::PlantObservation observation;
        if (source) { source->plantObservation(observation); }
        if (observation.reset_generation != generation || seed != last_seed) {
            estimator.reset(); rng = seed; last_seed = seed; generation = observation.reset_generation;
        }
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const double offset = noise * (static_cast<double>(rng)/4294967295.0*2.0 - 1.0);
        // Modulo in floating point before the integer cast avoids undefined
        // out-of-range conversion and preserves int32 encoder wrap.
        double counts = std::fmod(std::round(observation.position * static_cast<double>(cpr) * ratio /
            6.28318530717958647692 + offset), 4294967296.0);
        if (counts < 0) { counts += 4294967296.0; }
        m_measurement.count = std::bit_cast<int32_t>(static_cast<uint32_t>(counts));
        m_measurement.position = mod_motor::countsToRadians(m_measurement.count, cpr, ratio);
        m_measurement.velocity = estimator.update(m_measurement.count, t.dt_s, cpr, ratio, cutoff);
    }
    void onArmTransition(bool) override { estimator.reset(); }
    core::Status runAction(uint16_t id, const core::ActionArgs &, core::ActionOut &out) override {
        if (id != defs::feedback_mock::kSample) { return core::Status::fail(core::Result::InvalidArgument,core::Detail::UnknownRef); }
        out.values[0] = defs::f32Bits(m_measurement.velocity);
        out.values[1] = defs::f32Bits(m_measurement.position); return {};
    }
private:
    mod_motor::Actuator *source = nullptr;
    mod_motor::VelocityEstimator estimator;
    uint32_t ref = 0, seed = 1, rng = 1, last_seed = 1, generation = 0;
    float cpr = 1440, ratio = 1, cutoff = 30, noise = 0;
};
class Imu final : public core::Instance {
public:
    bool start(core::InstanceContext &ctx) override { return ctx.bindParam(1,&rate) && ctx.bindSignal(256,&yaw); }
    void tick(const core::TickContext &t) override { yaw = std::remainder(yaw + rate*t.dt_s,6.283185307F); }
    core::Status runAction(uint16_t id, const core::ActionArgs &, core::ActionOut &out) override {
        if (id == 769) { yaw = 0; }
        else if (id == 768) { out.values[0] = defs::f32Bits(yaw); }
        else { return core::Status::fail(core::Result::InvalidArgument,core::Detail::UnknownRef); }
        return {};
    }
private:
    float rate = .1F,yaw = 0;
};
class Fusion final : public core::Instance {
public:
    bool start(core::InstanceContext &ctx) override {
        return ctx.bindSource(defs::fusion_mock::kFeedback, &feedback) &&
               ctx.bindSource(defs::fusion_mock::kImu, &imu) &&
               ctx.bindSignal(defs::fusion_mock::kCombined, &combined);
    }
    void tick(const core::TickContext &) override {
        combined = (feedback.valid ? feedback.value : 0.F) + (imu.valid ? imu.value : 0.F);
    }
private:
    core::Sampled<float> feedback, imu;
    float combined = 0;
};
template<class T> core::Instance *make(const core::InstanceConfig &) { return new T; }
const core::ModuleType actuator{defs::actuator_mock::kDesc,make<Actuator>};
const core::ModuleType feedback{defs::feedback_mock::kDesc,make<Feedback>};
const core::ModuleType imu{defs::imu_mock::kDesc,make<Imu>};
const core::ModuleType fusion{defs::fusion_mock::kDesc,make<Fusion>};
}
const std::array<const core::ModuleType *,4> kTypes{&actuator,&feedback,&imu,&fusion};
}
