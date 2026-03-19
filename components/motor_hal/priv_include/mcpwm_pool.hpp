#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/mcpwm_prelude.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esp_motor {
namespace detail {

/// Identifies a motor's allocated MCPWM channel
struct McpwmChannel {
    int group_id;
    int oper_index;
    int slot;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator;
    mcpwm_gen_handle_t generator;
    uint32_t period_ticks;
};

/// Static resource manager for MCPWM hardware allocation.
/// Tracks available generators across all MCPWM groups/operators.
class McpwmPool {
public:
    /// Allocate an MCPWM channel for a motor PWM output.
    /// Creates timer/operator lazily if not yet initialized.
    /// @param pwm_gpio  GPIO number for the PWM output
    /// @param out       Filled with allocated channel info on success
    /// @return ESP_OK on success, ESP_ERR_NOT_FOUND if no free slots
    static esp_err_t allocate(int pwm_gpio, McpwmChannel& out);

    /// Release an MCPWM channel. Tears down operator/timer when
    /// they have no remaining users.
    static esp_err_t deallocate(const McpwmChannel& ch);

private:
    McpwmPool() = delete;

    static constexpr int kNumGroups = 2;
    static constexpr int kOpersPerGroup = 3;
    static constexpr int kSlotsPerOper = 2;

    struct OperatorState {
        mcpwm_oper_handle_t handle = nullptr;
        int user_count = 0;
    };

    struct GroupState {
        mcpwm_timer_handle_t timer = nullptr;
        bool timer_enabled = false;
        int total_users = 0;
        OperatorState opers[kOpersPerGroup] = {};
    };

    static GroupState s_groups[kNumGroups];
    static SemaphoreHandle_t s_mutex;
    static bool s_initialized;

    static void ensureInitialized();
    static esp_err_t ensureTimer(int group_id);
    static esp_err_t ensureOperator(int group_id, int oper_index);
    static bool findFreeSlot(int& group_id, int& oper_index, int& slot);
};

} // namespace detail
} // namespace esp_motor
