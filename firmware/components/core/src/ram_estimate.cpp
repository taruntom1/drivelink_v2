// The RAM estimate behind the capacity limits (core/ram_estimate.hpp). Pure.

#include "core/ram_estimate.hpp"

#include "core/capture_service.hpp"
#include "core/core.hpp"
#include "core/group_service.hpp"
#include "core/loop.hpp"
#include "core/pending_layout.hpp"
#include "core/testgen_service.hpp"
#include "core_os/periodic_task.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"

namespace core {

namespace {

/// The heap's own bytes per allocation (header and alignment).
constexpr size_t kAlloc = 16;
/// A loop task beyond its stack: TCB, timer, watchdog entry, body.
constexpr size_t kLoopTask = 1024;
/// One wired source: its triple buffer slot, binding, sink and vector slack,
/// and a whole link of its own.
constexpr size_t kSourceLink = 512;
/// Copies of a layout entry that live at once while a pending session edits
/// and saves: before, after, the layout being written.
constexpr size_t kConfigCopies = 4;

/// The record of an instance of `type`, without its object.
size_t recordBytes(const defs::TypeDesc &type)
{
    return sizeof(detail::InstanceRecord) + (type.items.size() * (sizeof(detail::Binding) + sizeof(uint32_t))) +
           (4U * kAlloc);
}

size_t stackBytes(const LayoutEntry &loop)
{
    for (size_t k = 0; k < loop.desc->items.size(); ++k) {
        if (loop.desc->items[k].id == defs::loop::kStackBytes) {
            return loop.values[k];
        }
    }
    return 0;
}

} // namespace

size_t ramLimit(const CoreConfig &config)
{
    const RamBudget &ram = config.ram;
    return (ram.heap_bytes > ram.reserve_bytes) ? ram.heap_bytes - ram.reserve_bytes : 0U;
}

size_t estimateRam(const CoreConfig &config, std::span<const LayoutEntry> entries)
{
    const CoreConfig c = normalizedConfig(config);
    size_t total = sizeof(Core) + GroupService::baseBytes() + (8U * kAlloc) + c.ram.after_core_bytes;
    total += recordBytes(*c.system_type);

    // Sized by the limits alone.
    total += (static_cast<size_t>(c.subscription_groups) + c.command_groups) *
             (GroupService::slotBytes() + sizeof(void *) + kAlloc);
    total += c.subscription_groups *
             (c.ram.stream_slot_bytes + (2U * (kGroupStreamPrefix + (4U * c.group_entries))) + (4U * kAlloc));
    total += c.max_loops * (sizeof(core_os::PeriodicTask) + TestgenService::slotBytes());
    total += (static_cast<size_t>(c.max_instances) + c.max_loops) * 2U * sizeof(void *);

    // Sized by the layout, and by the limits it runs with.
    for (size_t i = 0; i < entries.size(); ++i) {
        const LayoutEntry &e = entries[i];
        if (e.desc == nullptr) {
            continue;
        }
        total += recordBytes(*e.desc);
        total += kConfigCopies * (sizeof(PendingEntry) + (e.desc->items.size() * sizeof(ParamPair)) + (2U * kAlloc));
        if (e.type_id == defs::kLoopTypeId) {
            total += sizeof(Loop) + (c.max_instances * 2U * sizeof(void *)) + kLoopTask + stackBytes(e) + (4U * kAlloc);
        } else {
            total += kModuleRamBytes + (e.desc->items.size() * Loop::inputBytes()) + kAlloc;
        }
        for (const RefLink &link : sourceLinks(entries, i)) {
            (void)link;
            total += kSourceLink;
        }
    }
    return total;
}

} // namespace core
