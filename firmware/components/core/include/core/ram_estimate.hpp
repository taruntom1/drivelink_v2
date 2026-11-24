#pragma once

#include <cstddef>
#include <span>

#include "core/core_config.hpp"
#include "core/layout_validation.hpp"

namespace core {

/// The internal heap a device running `entries` with these limits takes after
/// boot, in bytes: everything the limits and the layout size, plus
/// `RamBudget::after_core_bytes` and the config copies of a pending session.
/// Conservative: a flat `kModuleRamBytes` per module, and every wired source
/// counts as its own loop link. Pure, configure time.
size_t estimateRam(const CoreConfig &config, std::span<const LayoutEntry> entries);

/// What `estimateRam` may reach: the heap that was free before Core was built,
/// minus the reserve.
size_t ramLimit(const CoreConfig &config);

/// What one module instance may allocate itself (its object, driver handles).
inline constexpr size_t kModuleRamBytes = 2048;

} // namespace core
