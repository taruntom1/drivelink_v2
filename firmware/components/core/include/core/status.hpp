#pragma once

#include <cstdint>
#include "drivelink/protocol/gen/registry.hpp"

namespace core {

/// Shared wire result and detail enums, generated from registry.yaml.
using Result = drivelink::protocol::defs::Result;

using Detail = drivelink::protocol::defs::Detail;

/// A result, the detail, and the index of the failing entry in a batch (0
/// when the operation has no entries).
struct Status {
    Result result = Result::Ok;
    Detail detail = Detail::None;
    uint16_t index = 0;

    constexpr bool ok() const { return result == Result::Ok; }
    /// Ok, or PendingReboot: the operation was accepted (a pending one takes
    /// effect after SAVE and a reboot).
    constexpr bool accepted() const { return result == Result::Ok || result == Result::PendingReboot; }

    static constexpr Status success() { return Status{}; }
    static constexpr Status fail(Result result, Detail detail, uint16_t index = 0)
    {
        return Status{result, detail, index};
    }
};

} // namespace core
