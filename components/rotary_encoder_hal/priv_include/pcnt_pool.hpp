#pragma once

#include <cstdint>
#include "esp_err.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esp_rotary_encoder {
namespace detail {

/// Identifies an allocated PCNT unit configured for quadrature decoding.
/// Holds all handles needed to read counts and to tear down the unit.
struct PcntUnit {
    pcnt_unit_handle_t unit;         ///< PCNT unit handle
    pcnt_channel_handle_t channel_a; ///< Channel watching signal A edges
    pcnt_channel_handle_t channel_b; ///< Channel watching signal B edges
};

/// Static resource manager for PCNT hardware allocation.
/// Wraps the multi-step PCNT setup/teardown sequence for quadrature encoders.
/// Thread-safe via FreeRTOS mutex (same pattern as McpwmPool).
class PcntPool {
public:
    /// Allocate a PCNT unit and configure it for quadrature decoding.
    /// Performs the full setup sequence: create unit, glitch filter,
    /// create both channels with quadrature actions, add watch points,
    /// enable accumulator, enable unit, clear count, start unit.
    /// @param pin_a  GPIO for encoder channel A
    /// @param pin_b  GPIO for encoder channel B
    /// @param out    Filled with allocated handles on success
    /// @return ESP_OK on success, ESP_ERR_NOT_FOUND if all 4 units are in use
    static esp_err_t allocate(gpio_num_t pin_a, gpio_num_t pin_b, PcntUnit &out);

    /// Release a PCNT unit. Performs the full teardown sequence:
    /// stop unit, disable unit, remove watch points, delete channels, delete unit.
    /// @param unit  The unit to release (from a previous allocate() call)
    /// @return ESP_OK on success
    static esp_err_t deallocate(const PcntUnit &unit);

private:
    PcntPool() = delete; // static-only class, no instances

    static constexpr int kMaxUnits = 4; // SOC_PCNT_UNITS_PER_GROUP on ESP32-S3

    static SemaphoreHandle_t s_mutex;
    static bool s_initialized;
    static int s_active_count;

    static void ensureInitialized();
};

} // namespace detail
} // namespace esp_rotary_encoder
