#pragma once
#include "core_os/detail/backend_types.hpp"
#include "core_os/types.hpp"
namespace core_os {
// A callback on esp_timer's task (not a loop task). Setup/stop are owned by
// the config caller. stop() fences callbacks before releasing their context.
class PeriodicTimer {
public:
    ~PeriodicTimer();
    Err start(const char *name, uint32_t period_us, OneShotFn fn, void *ctx);
    void stop();
private:
    detail::PeriodicTimerState m_state;
};
}
