#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/core_config.hpp"
#include "core/module_type.hpp"
#include "core/persistence.hpp"
#include "core/safe_mode.hpp"
#include "core/status.hpp"
#include "core_os/kv_store.hpp"

namespace core {

class Core;

// The boot, in three stages, so that nothing is sized from a saved setting
// before the crash counter has gone up:
//   (a) prepareBoot(): the safe-mode decision, then the NVS load
//   (b) bootConfig():  CoreConfig from the saved settings
//   (c) construct Core, init(), boot(plan)
// `bringUp()` runs all three. main and vdev use it; a native test that needs a
// stage of its own calls the same functions.

/// What stage (a) decided and loaded. `Core::boot()` consumes it: the saved
/// generation is read from NVS once.
struct BootPlan {
    core_os::KvStore *store = nullptr;
    SafeModeDecision safe{};
    persist::LoadedConfig config;
};

/// Stage (a), once per boot. Stores the new crash counter **first**, so that a
/// crash anywhere after it counts (`core.SafeModeEntered` when it enters safe
/// mode), then loads the newest complete generation. `store` must outlive the
/// core.
BootPlan prepareBoot(core_os::KvStore &store);

/// The saved value of every system item, by item index of `type` (the default
/// where nothing is saved).
std::vector<uint32_t> savedSystemValues(const BootPlan &plan, const defs::TypeDesc &type);

/// The value of every system item this boot runs with: the saved one, but in
/// safe mode every `reboot_required` one is at its default, so a setting that
/// crashes the boot cannot crash safe mode too.
std::vector<uint32_t> runningSystemValues(const BootPlan &plan, const defs::TypeDesc &type);

/// Stage (b): `base` with the running system settings applied.
CoreConfig bootConfig(const BootPlan &plan, const CoreConfig &base);

/// What `bringUp()` made. On failure `core` is null and `stage` is the
/// `core.InitFailed` stage (`kInitStageRegistry` or `kInitStageBoot`).
struct BringUp {
    std::unique_ptr<Core> core;
    Status status;
    uint32_t stage = 0;
};

/// Stages (a) to (c): a booted core, ready for `start()`.
BringUp bringUp(core_os::KvStore &store, const CoreConfig &base, std::span<const ModuleType *const> types);

} // namespace core
