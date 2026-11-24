#pragma once

#include <cstdint>
#include <span>

#include "core/gpio_policy.hpp"
#include "drivelink/protocol/defs.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

namespace core {

struct CoreConfig;

/// Write the `reboot_required` system settings in `values` (by item index of
/// the config's `system_type`) into `config`. Each call sets every field it
/// owns. Pure.
using ConfigureSystemFn = void (*)(CoreConfig &config, std::span<const uint32_t> values);

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
    uint16_t link_values = 32;     ///< distinct target values per ordered loop pair, 1..64
    uint8_t capture_refs = 16;     ///< signals or inputs per capture, 1..64 (and within the STATUS reply)
    /// The largest capture ring; the device reports the smaller of this and
    /// the largest free PSRAM block (32 KB internal without PSRAM).
    uint32_t capture_max_bytes = 4U * 1024U * 1024U;
    /// The pins a `gpio` param may name: bit n allows GPIO n. On the device
    /// it is the ESP32-S3 policy minus the uart_comm TX/RX pins.
    uint64_t gpio_allowed_mask = gpio_policy::kEsp32s3Allowed;
    /// How long after a normal boot the crash counter is cleared (safe mode).
    uint32_t crash_clear_delay_us = 30000000;
    /// The system type's descriptor (type id 0x00). Only test setups replace
    /// it, with a copy of the generated one that has more items.
    const drivelink::protocol::defs::TypeDesc *system_type = &drivelink::protocol::defs::system::kDesc;
    /// How the `reboot_required` system settings size the core; nullptr while
    /// no generated system item is `reboot_required`.
    ConfigureSystemFn configure_system = nullptr;
};

/// `config` with every field clamped to what the core supports.
CoreConfig normalizedConfig(CoreConfig config);

/// `base` with the system settings in `values` applied (`configure_system`).
CoreConfig withSystemSettings(CoreConfig base, std::span<const uint32_t> values);

/// The config from Kconfig. Defined in `src/idf/core_config_idf.cpp`, which
/// only the IDF build compiles.
CoreConfig coreConfigFromKconfig();

} // namespace core
