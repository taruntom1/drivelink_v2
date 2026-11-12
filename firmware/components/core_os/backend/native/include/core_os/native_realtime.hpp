#pragma once
#include <cstdint>
namespace core_os::native {
using RealtimeViolationSink = void (*)(uint64_t count);
/// Native tests install a failure callback; vdev uses the cumulative counter.
/// Called after leaving the outer guard so reporting itself is not guarded.
RealtimeViolationSink setRealtimeViolationSink(RealtimeViolationSink sink);
}
