#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/status.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core {

namespace defs = drivelink::protocol::defs;

/// One entry of a layout, as validation and the wiring order see it. The
/// pending layout and the boot path each build these over their own storage.
struct LayoutEntry {
    const defs::TypeDesc *desc; ///< nullptr when the type is not registered
    uint8_t type_id;
    uint8_t instance;
    uint8_t loop;
    const char *label;      ///< NUL-terminated, never null
    const uint32_t *values; ///< param value bits by item index of `desc` (the pins and the wiring are read here)
};

/// What validation checks against.
struct ValidationConfig {
    uint64_t gpio_allowed_mask; ///< bit n: GPIO n may be claimed
    size_t max_instances;       ///< the layout's capacity
    size_t max_loops = 4;
};

inline constexpr size_t kNoEntry = static_cast<size_t>(-1);

/// The index of entry (type, instance) in `entries` (the first one), or kNoEntry.
size_t findEntry(std::span<const LayoutEntry> entries, uint8_t type_id, uint8_t instance);

/// One wiring link of an entry: a `ref` param with a non-zero value.
struct RefLink {
    size_t item;        ///< the ref param's item index
    size_t target;      ///< the entry it names, or kNoEntry
    bool ticks_after;   ///< `RefTicksAfter`: the target ticks after the entry
    uint32_t value;     ///< the raw value
};

/// The wiring links of entry `i` (configure time: it allocates).
std::vector<RefLink> refLinks(std::span<const LayoutEntry> entries, size_t i);

/// Validate a whole layout. `out[i]` (`out` at least as long as `entries`)
/// receives entry i's status: Ok, or the first check it failed. The checks,
/// in order:
///   - the type is registered (`NotFound` / `UnknownType`)
///   - there is room: index below `max_instances` (`NoMemory` / `Capacity`)
///   - (type, instance) is not used by an earlier entry (`Exists`)
///   - the loop is one the type allows (`LoopMismatch`)
///   - the label is non-empty, at most 16 characters (`BadLabel`) and not used
///     by an earlier entry (`LabelInUse`)
///   - every pin (`gpio` param other than `kNoGpio`) is allowed
///     (`GpioNotAllowed`) and not claimed by the entry itself or an earlier
///     valid entry (`GpioConflict`)
///   - every wiring value is 0, or names an existing entry of the ref's
///     category (`BadRef`) in the same loop (`LoopMismatch`)
///   - the entry is on no cycle of the ref graph or the tick graph
///     (`WiringCycle`)
///   - every entry it is wired to is valid (`DependencyMissing`, transitively)
/// All but `Exists`, `Capacity` and `UnknownType` fail as `InvalidArgument`.
/// Pure, configure time (it allocates). @return how many entries failed.
size_t validateLayout(std::span<const LayoutEntry> entries, const ValidationConfig &config, std::span<Status> out);

} // namespace core
