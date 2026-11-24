#pragma once

#include <cstddef>

namespace core_os {

/// The free internal heap, in bytes. Call it at setup only. Natively it is
/// unlimited unless a test sets it (`native::setFreeInternalBytes`).
size_t freeInternalBytes();

} // namespace core_os
