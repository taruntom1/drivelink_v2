#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/core_config.hpp"
#include "core/link_settings.hpp"
#include "core/module_type.hpp"
#include "core/persistence.hpp"
#include "core/safe_mode.hpp"
#include "core/status.hpp"
#include "core_os/kv_store.hpp"

namespace core {

class Core;

// The boot, in three stages, so that nothing is sized from a saved setting
// before the crash counter has gone up:
//   (a) countBoot():   the safe-mode decision
//       loadBoot():    the NVS load and the link of this boot
//   (b) bootConfig():  CoreConfig from the saved settings
//   (c) construct Core, init(), boot(plan)
// main runs (a), opens the UART on `plan.link.running` (`fallBackLink()` when it
// fails) and then `bringUp()`, which is (b) and (c). vdev does the same; a
// native test that needs a stage of its own calls the same functions.

/// What stage (a) decided and loaded. `Core::boot()` consumes it: the saved
/// generation is read from NVS once.
struct BootPlan {
    core_os::KvStore *store = nullptr;
    SafeModeDecision safe{};
    persist::LoadedConfig config;
    LinkBoot link;
    size_t loaded_bytes = 0; ///< the internal heap `config` takes until the boot is over
};

/// Stage (a), first half, once per boot and before anything else: stores the
/// new crash counter, so that a crash anywhere after it counts
/// (`core.SafeModeEntered` when it enters safe mode).
SafeModeDecision countBoot();

/// Stage (a), second half, after NVS is up: loads the newest complete
/// generation and decides the link (`bootLink()`; a saved trial is deferred in
/// safe mode and on an unconfirmed OTA image). `store` must outlive the core.
BootPlan loadBoot(core_os::KvStore &store, SafeModeDecision safe);

/// Stage (a): both halves.
BootPlan prepareBoot(core_os::KvStore &store);

/// The saved value of every system item, by item index of `type` (the default
/// where nothing is saved): what the next boot runs if nothing changes.
std::vector<uint32_t> savedSystemValues(const BootPlan &plan, const defs::TypeDesc &type);

/// The value of every system item this boot runs with: the saved one, but in
/// safe mode every `reboot_required` one is at its default, so a setting that
/// crashes the boot cannot crash safe mode too. The link is always `plan.link`'s.
std::vector<uint32_t> runningSystemValues(const BootPlan &plan, const defs::TypeDesc &type);

/// Stage (b): `base` with the running system settings applied, and the record
/// table sized for the saved layout.
CoreConfig bootConfig(const BootPlan &plan, const CoreConfig &base);

/// What `bringUp()` made. On failure `core` is null and `stage` is the
/// `core.InitFailed` stage (`kInitStageRegistry` or `kInitStageBoot`).
struct BringUp {
    std::unique_ptr<Core> core;
    Status status;
    uint32_t stage = 0;
};

/// Stages (b) and (c): a booted core, ready for `start()`. Records the free
/// internal heap first, for the RAM check of later settings.
BringUp bringUp(BootPlan plan, const CoreConfig &base, std::span<const ModuleType *const> types);

/// Stages (a) to (c).
BringUp bringUp(core_os::KvStore &store, const CoreConfig &base, std::span<const ModuleType *const> types);

} // namespace core
