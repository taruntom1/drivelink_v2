#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/mcpwm_prelude.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace motor_hal {
namespace detail {

/// A motor's MCPWM channel. A null handle was not created.
struct McpwmChannel {
    int group_id = 0;
    mcpwm_timer_handle_t timer = nullptr; ///< set only while enabled and started
    mcpwm_oper_handle_t oper = nullptr;
    mcpwm_cmpr_handle_t comparator = nullptr;
    mcpwm_gen_handle_t generator = nullptr;
    uint32_t period_ticks = 0;
};

/// Static resource manager for MCPWM hardware allocation.
/// Each motor takes an operator of its own (6 on the ESP32-S3): the two
/// generators of an operator share one force register, which the driver
/// updates read-modify-write, so a force set by one motor's emergency stop
/// could be undone by another motor's release on a different task. It also
/// takes a timer of its own, for its PWM frequency; a group has as many timers
/// as operators. All timers use one resolution, because the timers of a group
/// share its clock prescaler.
class McpwmPool {
public:
    /// Allocate an MCPWM channel for a motor PWM output.
    /// @param pwm_gpio      GPIO number for the PWM output
    /// @param pwm_freq_hz   PWM frequency, rounded to whole timer ticks
    /// @param out           Filled with the channel on success, else unchanged
    /// @param start_forced  Force the generator low before its PWM actions
    ///                      are set; it stays forced until
    ///                      mcpwm_generator_set_force_level(gen, -1, true).
    /// @return ESP_OK on success, ESP_ERR_NOT_FOUND if no operator is free,
    ///         ESP_ERR_INVALID_ARG for a frequency the timer cannot make, or
    ///         the driver error. A failure releases what it took.
    static esp_err_t allocate(int pwm_gpio, uint32_t pwm_freq_hz, McpwmChannel &out, bool start_forced);

    /// Force the output low and release the channel.
    static void deallocate(McpwmChannel &ch);

private:
    McpwmPool() = delete;

    static constexpr int kNumGroups = 2;
    static constexpr int kMotorsPerGroup = 3; // operators, and timers

    static int s_users[kNumGroups];

    static SemaphoreHandle_t poolMutex();
    static esp_err_t create(int pwm_gpio, bool start_forced, McpwmChannel &ch);
    static esp_err_t createTimer(McpwmChannel &ch);
    static void destroy(McpwmChannel &ch);
};

} // namespace detail
} // namespace motor_hal
