#pragma once

#include <cstdint>
#include "drivelink/protocol/gen/registry.hpp"
#include "sdkconfig.h"

namespace time_sync {

/// Default MSG_TYPE of the ESP-initiated sync request (generated registry).
constexpr uint8_t kDefaultRequestMsgType = drivelink::protocol::defs::msg::kTimeSyncRequest;
/// Default MSG_TYPE of the host's sync response (generated registry).
constexpr uint8_t kDefaultResponseMsgType = drivelink::protocol::defs::msg::kTimeSyncResponse;
/// Default periodic sync task stack size in bytes (Kconfig).
constexpr uint32_t kDefaultTaskStackSize = CONFIG_TIME_SYNC_TASK_STACK;
/// Default periodic sync task priority (Kconfig).
constexpr uint32_t kDefaultTaskPriority = CONFIG_TIME_SYNC_TASK_PRIO;

} // namespace time_sync
