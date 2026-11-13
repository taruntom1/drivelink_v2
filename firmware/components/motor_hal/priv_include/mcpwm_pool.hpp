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
    /// @param pwm_gpio            GPIO number for the PWM output
    /// @param out                 Filled with allocated channel info on success
    /// @param exclusive_operator  Take an operator with no other user and keep
    ///                            it to this channel. The two generators of an
    ///                            operator share one force register, so a
    ///                            channel that is forced and released from
    ///                            different tasks must not share it. Shared
    ///                            allocations never join an exclusive operator.
    /// @param start_forced        Force the generator low before its PWM
    ///                            actions are set; it stays forced until
    ///                            mcpwm_generator_set_force_level(gen, -1, true).
    /// @return ESP_OK on success, ESP_ERR_NOT_FOUND if no free slots
    static esp_err_t allocate(int pwm_gpio, McpwmChannel& out,
                              bool exclusive_operator = false, bool start_forced = false);

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
        bool exclusive = false;
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
    static bool findFreeSlot(bool exclusive, int& group_id, int& oper_index, int& slot);
};

} // namespace detail
} // namespace motor_hal
