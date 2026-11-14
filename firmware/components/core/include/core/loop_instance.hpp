#pragma once

#include "core/loop.hpp"
#include "core/module_type.hpp"
#include "core_os/types.hpp"

namespace core {

enum class LoopCore : uint8_t { Core0 = 0, Core1 = 1, Any = 2 };

/// Convert a configured placement to an OS affinity. Invalid choices fail
/// before creating a task; the enum's Any value is not an OS core id.
bool loopCoreAffinity(uint8_t placement, int8_t &affinity);

/// Built-in layout record; configuration is owned by the config task, and
/// signal storage is written at the end of this loop's tick.
class LoopInstance final : public Instance {
public:
    bool start(InstanceContext &ctx) override;
    void tick(const TickContext &) override {}
    uint16_t rate = 100;
    bool realtime = false;
    LoopCore core = LoopCore::Any;
    uint8_t priority = 10;
    uint16_t stack = 4096;
    LoopStats signals{};
};

const ModuleType &loopType();

} // namespace core
