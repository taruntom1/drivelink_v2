#pragma once
#include "drivelink/session.hpp"
// Deterministic wire peers exercise explicit pings separately. Production
// periodic ownership is covered by test_async and the real-vdev probe.
inline drivelink::SessionOptions testOptions() {
    drivelink::SessionOptions options;options.periodic_time=false;return options;
}
