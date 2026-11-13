#pragma once

#include <cstdint>

#include "core/gpio_policy.hpp"

namespace core {

/// The core's tunables. The core is pure and reads no Kconfig itself:
/// `coreConfigFromKconfig()` (IDF builds only) fills this from the
/// `CORE_*` options, and native builds fill it directly. The defaults here
/// match the Kconfig defaults.
struct CoreConfig {
    uint8_t subscription_groups = 8;
    uint8_t command_groups = 8;
    uint8_t group_entries = 64;
    uint8_t max_loops = 4;        ///< simultaneously configured loops, 1..8
    uint16_t max_instances = 16;   ///< module instances across all loops (loop capacity is separate)
    /// The pins a `gpio` param may name: bit n allows GPIO n. On the device
    /// it is the ESP32-S3 policy minus the uart_comm TX/RX pins.
    uint64_t gpio_allowed_mask = gpio_policy::kEsp32s3Allowed;
    /// How long after a normal boot the crash counter is cleared (safe mode).
    uint32_t crash_clear_delay_us = 30000000;
};

/// The config from Kconfig. Defined in `src/idf/core_config_idf.cpp`, which
/// only the IDF build compiles.
CoreConfig coreConfigFromKconfig();

} // namespace core
