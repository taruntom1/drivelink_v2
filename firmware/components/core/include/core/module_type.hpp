#pragma once

#include <cstdint>

#include "drivelink/protocol/defs.hpp"

namespace core {

class Instance;

/// What `ModuleType::create` gets: which instance to make. Boot only.
struct InstanceConfig {
    const drivelink::protocol::defs::TypeDesc *desc;
    uint8_t type_id;
    uint8_t instance;
    uint8_t loop;      ///< 0 control, 1 sensor
    const char *label; ///< up to 16 characters, never null
};

/// One module type: its generated descriptor and its factory.
///
/// main passes the list of module types to `Core::init()` explicitly (no
/// static self-registration, which the linker would drop). `desc` must be the
/// generated `kTypeTable` entry for its type id.
struct ModuleType {
    const drivelink::protocol::defs::TypeDesc &desc;
    /// Make an instance, or return nullptr. Called at boot only; may
    /// allocate. The core owns the result and deletes it.
    Instance *(*create)(const InstanceConfig &cfg);
};

} // namespace core
