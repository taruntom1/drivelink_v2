// IDF only: the core's Kconfig options into CoreConfig. The rest of the core
// never sees sdkconfig.h or a CONFIG_ symbol.

#include "core/core_config.hpp"
#include "sdkconfig.h"

namespace core {

CoreConfig coreConfigFromKconfig()
{
    CoreConfig cfg;
    cfg.control = {CONFIG_CORE_CONTROL_TASK_STACK, CONFIG_CORE_CONTROL_TASK_PRIO, CONFIG_CORE_CONTROL_TASK_CORE};
    cfg.sensor = {CONFIG_CORE_SENSOR_TASK_STACK, CONFIG_CORE_SENSOR_TASK_PRIO, CONFIG_CORE_SENSOR_TASK_CORE};
    cfg.max_instances = CONFIG_CORE_MAX_INSTANCES;
    // A module must never take over the host link.
    cfg.gpio_allowed_mask = gpio_policy::kEsp32s3Allowed &
                            ~(gpio_policy::gpioBit(CONFIG_UART_COMM_TX_PIN) | gpio_policy::gpioBit(CONFIG_UART_COMM_RX_PIN));
    return cfg;
}

} // namespace core
