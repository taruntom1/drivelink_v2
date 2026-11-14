#include "core/loop_instance.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"

namespace core {
namespace lp = drivelink::protocol::defs::loop;

bool loopCoreAffinity(uint8_t placement, int8_t &affinity)
{
    switch (static_cast<LoopCore>(placement)) {
    case LoopCore::Core0: affinity = 0; return true;
    case LoopCore::Core1: affinity = 1; return true;
    case LoopCore::Any: affinity = core_os::kNoAffinity; return true;
    }
    return false;
}

bool LoopInstance::start(InstanceContext &ctx)
{
    ctx.bindParam(lp::kRateHz, &rate);
    ctx.bindParam(lp::kRealtime, &realtime);
    ctx.bindParam(lp::kCore, &core);
    ctx.bindParam(lp::kPriority, &priority);
    ctx.bindParam(lp::kStackBytes, &stack);
    ctx.bindSignal(lp::kTicks, &signals.ticks);
    ctx.bindSignal(lp::kOverruns, &signals.overruns);
    ctx.bindSignal(lp::kLastRunUs, &signals.last_run_us);
    ctx.bindSignal(lp::kMaxRunUs, &signals.max_run_us);
    return true;
}

const ModuleType &loopType()
{
    static const ModuleType type{lp::kDesc, [](const InstanceConfig &) -> Instance * { return new LoopInstance; }};
    return type;
}
} // namespace core
