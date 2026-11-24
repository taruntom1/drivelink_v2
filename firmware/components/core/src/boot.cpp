// The boot stages before and around Core (core/boot.hpp). Pure; startup only.

#include "core/boot.hpp"

#include <utility>

#include "core/core.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

namespace core {

BootPlan prepareBoot(core_os::KvStore &store)
{
    BootPlan plan;
    plan.store = &store;
    // The counter first: anything after it may crash on a saved setting.
    plan.safe = decideSafeMode(core_os::loadCrashCount(), core_os::resetWasCrash());
    core_os::storeCrashCount(plan.safe.counter);
    if (plan.safe.safe_mode) {
        core_os::reportDiag(drivelink::protocol::defs::diag::codes::core::kSafeModeEntered, 0, plan.safe.counter, 0);
    }
    plan.config = persist::loadConfig(store);
    return plan;
}

std::vector<uint32_t> savedSystemValues(const BootPlan &plan, const defs::TypeDesc &type)
{
    std::vector<uint32_t> values(type.items.size());
    persist::applyPairs(type, plan.config.system, values.data()); // no pairs unless a generation was complete
    return values;
}

std::vector<uint32_t> runningSystemValues(const BootPlan &plan, const defs::TypeDesc &type)
{
    std::vector<uint32_t> values = savedSystemValues(plan, type);
    for (size_t i = 0; plan.safe.safe_mode && i < values.size(); ++i) {
        if (defs::hasFlag(type.items[i], defs::ItemFlag::RebootRequired)) {
            values[i] = type.items[i].default_bits;
        }
    }
    return values;
}

CoreConfig bootConfig(const BootPlan &plan, const CoreConfig &base)
{
    return withSystemSettings(base, runningSystemValues(plan, *base.system_type));
}

BringUp bringUp(core_os::KvStore &store, const CoreConfig &base, std::span<const ModuleType *const> types)
{
    BringUp up;
    BootPlan plan = prepareBoot(store);
    auto core = std::make_unique<Core>(bootConfig(plan, base));
    up.status = core->init(types);
    if (!up.status.ok()) {
        up.stage = kInitStageRegistry;
        return up;
    }
    up.status = core->boot(std::move(plan));
    if (!up.status.ok()) {
        up.stage = kInitStageBoot;
        return up;
    }
    up.core = std::move(core);
    return up;
}

} // namespace core
