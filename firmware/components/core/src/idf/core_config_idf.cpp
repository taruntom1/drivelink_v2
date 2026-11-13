// IDF only: the core's Kconfig options into CoreConfig. The rest of the core
// never sees sdkconfig.h or a CONFIG_ symbol.

#include "core/core_config.hpp"
#include "sdkconfig.h"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/core_codec.hpp"

static_assert(CONFIG_CORE_COMMAND_GROUPS <= drivelink::protocol::defs::kCmdGroupChannelCount);
static_assert(CONFIG_CORE_SUBSCRIPTION_GROUPS <= drivelink::protocol::defs::kSubGroupChannelCount);

namespace core {

CoreConfig coreConfigFromKconfig()
{
    CoreConfig cfg;
    cfg.subscription_groups = CONFIG_CORE_SUBSCRIPTION_GROUPS;
    cfg.command_groups = CONFIG_CORE_COMMAND_GROUPS;
    cfg.group_entries = drivelink::protocol::core_wire::groupEntryCapacity(
        CONFIG_UART_COMM_MAX_PAYLOAD, CONFIG_CORE_GROUP_ENTRIES);
    cfg.max_loops = CONFIG_CORE_MAX_LOOPS;
    cfg.max_instances = CONFIG_CORE_MAX_INSTANCES;
    // A module must never take over the host link.
    cfg.gpio_allowed_mask = gpio_policy::kEsp32s3Allowed &
                            ~(gpio_policy::gpioBit(CONFIG_UART_COMM_TX_PIN) | gpio_policy::gpioBit(CONFIG_UART_COMM_RX_PIN));
    return cfg;
}

} // namespace core
