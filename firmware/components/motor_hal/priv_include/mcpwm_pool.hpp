#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/mcpwm_prelude.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace motor_hal {
namespace detail {

/// Identifies a motor's allocated MCPWM channel
struct McpwmChannel {
    int group_id;
    int oper_index;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator;
    mcpwm_gen_handle_t generator;
    uint32_t period_ticks;
};

/// Static resource manager for MCPWM hardware allocation.
/// Each motor takes an operator of its own (6 on the ESP32-S3): the two
/// generators of an operator share one force register, which the driver
/// updates read-modify-write, so a force set by one motor's emergency stop
/// could be undone by another motor's release on a different task.
class McpwmPool {
public:
    /// Allocate an MCPWM channel for a motor PWM output.
    /// Creates timer/operator lazily if not yet initialized.
    /// @param pwm_gpio      GPIO number for the PWM output
    /// @param out           Filled with allocated channel info on success
    /// @param start_forced  Force the generator low before its PWM actions
    ///                      are set; it stays forced until
    ///                      mcpwm_generator_set_force_level(gen, -1, true).
    /// @return ESP_OK on success, ESP_ERR_NOT_FOUND if no operator is free
    static esp_err_t allocate(int pwm_gpio, McpwmChannel& out, bool start_forced);

    /// Release an MCPWM channel. Tears down operator/timer when
    /// they have no remaining users.
    static esp_err_t deallocate(const McpwmChannel& ch);

private:
    McpwmPool() = delete;

    static constexpr int kNumGroups = 2;
    static constexpr int kOpersPerGroup = 3;

    struct OperatorState {
        mcpwm_oper_handle_t handle = nullptr;
        bool in_use = false;
    };

    struct GroupState {
        mcpwm_timer_handle_t timer = nullptr;
        bool timer_enabled = false;
        int total_users = 0;
        OperatorState opers[kOpersPerGroup] = {};
    };

    static GroupState s_groups[kNumGroups];

    static SemaphoreHandle_t poolMutex();
    static esp_err_t ensureTimer(int group_id);
    static esp_err_t ensureOperator(int group_id, int oper_index);
    static bool findFreeOperator(int& group_id, int& oper_index);
};

} // namespace detail
} // namespace motor_hal
