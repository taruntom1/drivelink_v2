#include "mod_mock/modules.hpp"
#include "core/instance.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/type_actuator_mock.hpp"
#include "drivelink/protocol/gen/type_feedback_mock.hpp"
#include "drivelink/protocol/gen/type_imu_mock.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>

namespace mod_mock {
namespace {
namespace defs = drivelink::protocol::defs;
class Actuator final : public core::Instance {
public:
    bool start(core::InstanceContext &ctx) override {
        instance_id = ctx.instanceId();
        return ctx.bindParam(1,&tau) && ctx.bindParam(2,&gain) &&
            ctx.bindSignal(256,&velocity) && ctx.bindSignal(257,&position) && ctx.bindInput(512,&duty);
    }
    void tick(const core::TickContext &t) override {
        const float target = stopped.load(std::memory_order_acquire) ? 0.F : std::clamp(duty,-1.F,1.F)*gain;
        const float decay = std::exp(-t.dt_s/tau);
        position += target*t.dt_s + (velocity-target)*tau*(1.F-decay);
        velocity = target+(velocity-target)*decay;
    }
    core::Status runAction(uint16_t id, const core::ActionArgs &in, core::ActionOut &out) override {
        if (id == 768) {
            duty = std::clamp(defs::f32FromBits(in.values[0]),-1.F,1.F);
            out.values[0] = defs::f32Bits(velocity); out.values[1] = defs::f32Bits(position);
        } else if (id == 769) {
            velocity = 0; position = 0; duty = 0;
            out.blob_size = in.blob.size(); std::copy(in.blob.begin(),in.blob.end(),out.blob.begin());
        } else if (id == 770) {
            const auto code = in.values[0] ? defs::diag::codes::actuator_mock::kFaultInjected : defs::diag::codes::actuator_mock::kArmBlocked;
            if (in.values[1]) { core_os::raiseDiag(code, instance_id); } else { core_os::clearDiag(code, instance_id); }
        } else { return core::Status::fail(core::Result::InvalidArgument,core::Detail::UnknownRef); }
        return {};
    }
    float velocity = 0, position = 0;
    void onArmTransition(bool armed) override { duty = 0; stopped.store(!armed, std::memory_order_release); }
    void emergencyStop() override { stopped.store(true, std::memory_order_release); }
private:
    std::atomic<bool> stopped{true};
    uint8_t instance_id = 255;
    float tau = .1F, gain = 10, duty = 0;
};
class Feedback final : public core::Instance {
public:
    bool start(core::InstanceContext &ctx) override {
        const auto wired=ctx.param(1);
        if(wired && defs::wiringRefType(wired)!=defs::actuator_mock::kTypeId) { ctx.fail(1,-1);return false; }
        source = static_cast<Actuator *>(ctx.ref(1));
        // The only actuator type shipped in F3b is actuator.mock. Real
        // actuator interfaces replace this concrete dependency in F5.
        return ctx.bindParam(1,&ref) && ctx.bindSignal(256,&velocity) && ctx.bindSignal(257,&position);
    }
    void tick(const core::TickContext &) override {
        velocity = source ? source->velocity : 0; position = source ? source->position : 0;
    }
    core::Status runAction(uint16_t id, const core::ActionArgs &, core::ActionOut &out) override {
        if (id != 768) { return core::Status::fail(core::Result::InvalidArgument,core::Detail::UnknownRef); }
        out.values[0] = defs::f32Bits(velocity); out.values[1] = defs::f32Bits(position); return {};
    }
private:
    Actuator *source = nullptr;
    uint32_t ref = 0;
    float velocity = 0,position = 0;
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
template<class T> core::Instance *make(const core::InstanceConfig &) { return new T; }
const core::ModuleType actuator{defs::actuator_mock::kDesc,make<Actuator>};
const core::ModuleType feedback{defs::feedback_mock::kDesc,make<Feedback>};
const core::ModuleType imu{defs::imu_mock::kDesc,make<Imu>};
}
const std::array<const core::ModuleType *,3> kTypes{&actuator,&feedback,&imu};
}
