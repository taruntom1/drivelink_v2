#pragma once

#include "core/loop.hpp"
#include "core/module_type.hpp"

namespace core {

/// Built-in layout record; configuration is owned by the config task, and
/// signal storage is written at the end of this loop's tick.
class LoopInstance final : public Instance {
public:
    bool start(InstanceContext &ctx) override;
    void tick(const TickContext &) override {}
    uint16_t rate = 100;
    bool realtime = false;
    uint8_t core = 0;
    uint8_t priority = 10;
    uint16_t stack = 4096;
    LoopStats signals{};
};

const ModuleType &loopType();

} // namespace core
