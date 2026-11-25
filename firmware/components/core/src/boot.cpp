// The boot stages before and around Core (core/boot.hpp). Pure; startup only.

#include "core/boot.hpp"

#include <utility>

#include "core/core.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/memory.hpp"
#include "core_os/ota.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

namespace core {

SafeModeDecision countBoot()
{
    const SafeModeDecision safe = decideSafeMode(core_os::loadCrashCount(), core_os::resetWasCrash());
    core_os::storeCrashCount(safe.counter);
    if (safe.safe_mode) {
        core_os::reportDiag(drivelink::protocol::defs::diag::codes::core::kSafeModeEntered, 0, safe.counter, 0);
    }
    return safe;
}

BootPlan loadBoot(core_os::KvStore &store, SafeModeDecision safe)
{
    BootPlan plan;
    plan.store = &store;
    plan.safe = safe;
    const size_t before = core_os::freeInternalBytes();
    plan.config = persist::loadConfig(store);
    const size_t after = core_os::freeInternalBytes();
    plan.loaded_bytes = (before > after) ? before - after : 0U;
    plan.link = bootLink(store, safe.safe_mode || core_os::otaImageUnconfirmed());
    return plan;
}

BootPlan prepareBoot(core_os::KvStore &store)
{
    // The counter first: anything after it may crash on a saved setting.
    const SafeModeDecision safe = countBoot();
    return loadBoot(store, safe);
}

std::vector<uint32_t> savedSystemValues(const BootPlan &plan, const defs::TypeDesc &type)
{
    std::vector<uint32_t> values(type.items.size());
    persist::applyPairs(type, plan.config.system, values.data()); // no pairs unless a generation was complete
    linkInto(type, plan.link.next(), values);
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
    linkInto(type, plan.link.running, values);
    return values;
}

CoreConfig bootConfig(const BootPlan &plan, const CoreConfig &base)
{
    CoreConfig config = withSystemSettings(base, runningSystemValues(plan, *base.system_type));
    config.saved_entries = plan.config.instances.size();
    // Besides the running link's pins, the ones a later boot may come up on.
    config.gpio_allowed_mask &= ~(plan.link.records.confirmed.pinMask() | plan.link.next().pinMask());
    return config;
}

BringUp bringUp(BootPlan plan, const CoreConfig &base, std::span<const ModuleType *const> types)
{
    BringUp up;
    CoreConfig measured = base;
    // The loaded configuration is freed when the boot is over.
    const size_t heap = core_os::freeInternalBytes();
    measured.ram.heap_bytes = (heap > SIZE_MAX - plan.loaded_bytes) ? SIZE_MAX : heap + plan.loaded_bytes;
    auto core = std::make_unique<Core>(bootConfig(plan, measured));
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

BringUp bringUp(core_os::KvStore &store, const CoreConfig &base, std::span<const ModuleType *const> types)
{
    return bringUp(prepareBoot(store), base, types);
}

} // namespace core
