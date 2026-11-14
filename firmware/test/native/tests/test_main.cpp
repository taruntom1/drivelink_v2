#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "core_os/native_realtime.hpp"

namespace {
const bool s_guard_policy = [] {
    core_os::native::setRealtimeViolationSink([](uint64_t count) {
        FAIL_CHECK("Real-time tick attempted allocation or a blocking core_os operation: " << count);
    });
    return true;
}();
}
