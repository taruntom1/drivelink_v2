#pragma once

#include <cstdint>

namespace core {

/// The core's tunables. The core is pure and reads no Kconfig itself:
/// `coreConfigFromKconfig()` (IDF builds only) fills this from the
/// `CORE_*` options, and native builds fill it directly. The defaults here
/// match the Kconfig defaults.
struct CoreConfig {
    struct LoopTask {
        uint32_t stack_bytes;
        uint8_t priority;
        int8_t core; ///< 0, 1 or -1 (no affinity)
    };

    LoopTask control{4096, 20, 1}; ///< the control loop task, "core_ctrl"
    LoopTask sensor{4096, 10, 0};  ///< the sensor loop task, "core_sens"
    uint16_t max_instances = 16;   ///< module instances, across both loops
};

/// The config from Kconfig. Defined in `src/idf/core_config_idf.cpp`, which
/// only the IDF build compiles.
CoreConfig coreConfigFromKconfig();

} // namespace core
