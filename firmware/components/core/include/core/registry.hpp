#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/module_type.hpp"
#include "core/status.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core {

/// The module types this firmware can instantiate, as main lists them.
///
/// The list is passed in explicitly and must outlive the registry (main keeps
/// it in a static array). Lookup is a linear scan: there are only a handful.
class Registry {
public:
    /// Take the list. Rejects a null entry (`UnknownType`), type ids 0x00 and 0x05
    /// (`ReservedType`), a null `create` (`NoFactory`) and a type id listed
    /// twice (`DuplicateType`), all as `InvalidArgument` with the entry's
    /// index. On failure the registry stays empty.
    Status init(std::span<const ModuleType *const> types);

    /// The module type with this id, or nullptr.
    const ModuleType *find(uint8_t type_id) const;

    size_t size() const { return m_types.size(); }

private:
    std::span<const ModuleType *const> m_types;
};

/// True if `type.desc` is the generated table entry for its type id (main
/// checks each module type against `defs::kTypeTable`).
bool descIsInTable(const ModuleType &type, std::span<const drivelink::protocol::defs::TypeDesc *const> table);

} // namespace core
