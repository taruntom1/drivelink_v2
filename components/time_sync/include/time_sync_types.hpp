#pragma once

#include <cstdint>
#include "sdkconfig.h"

namespace time_sync {

/// Default MSG_TYPE of the ESP-initiated sync request (Kconfig).
constexpr uint8_t kDefaultRequestMsgType = CONFIG_TIME_SYNC_REQUEST_MSG_TYPE;
/// Default MSG_TYPE of the host's sync response (Kconfig).
constexpr uint8_t kDefaultResponseMsgType = CONFIG_TIME_SYNC_RESPONSE_MSG_TYPE;
/// Default periodic sync task stack size in bytes (Kconfig).
constexpr uint32_t kDefaultTaskStackSize = CONFIG_TIME_SYNC_TASK_STACK;
/// Default periodic sync task priority (Kconfig).
constexpr uint32_t kDefaultTaskPriority = CONFIG_TIME_SYNC_TASK_PRIO;

} // namespace time_sync
