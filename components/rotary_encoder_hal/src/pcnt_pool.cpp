#include "pcnt_pool.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"

static const char *TAG = "pcnt_pool";

namespace esp_rotary_encoder {
namespace detail {

int PcntPool::s_active_count = 0;

SemaphoreHandle_t PcntPool::poolMutex()
{
    // Function-local static: C++ guarantees one-time, thread-safe
    // initialisation (ESP-IDF implements the guard with FreeRTOS), so two
    // tasks creating their first encoder at once can't create two mutexes.
    static const SemaphoreHandle_t mutex = [] {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        assert(m != nullptr);
        return m;
    }();
    return mutex;
}

esp_err_t PcntPool::allocate(gpio_num_t pin_a, gpio_num_t pin_b, PcntUnit &out)
{
    const SemaphoreHandle_t mtx = poolMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);

    if (s_active_count >= kMaxUnits) {
        xSemaphoreGive(mtx);
        ESP_LOGE(TAG, "No free PCNT units (max %d encoders)", kMaxUnits);
        return ESP_ERR_NOT_FOUND;
    }

    // Create PCNT unit with accumulator enabled
    pcnt_unit_config_t unit_cfg = {};
    unit_cfg.high_limit = CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT;
    unit_cfg.low_limit = CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT;
    unit_cfg.flags.accum_count = true;

    esp_err_t ret = pcnt_new_unit(&unit_cfg, &out.unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PCNT unit: %s", esp_err_to_name(ret));
        xSemaphoreGive(mtx);
        return ret;
    }

    // Apply glitch filter if configured
#if CONFIG_ENCODER_HAL_GLITCH_FILTER_NS > 0
    pcnt_glitch_filter_config_t filter_cfg = {};
    filter_cfg.max_glitch_ns = CONFIG_ENCODER_HAL_GLITCH_FILTER_NS;
    ret = pcnt_unit_set_glitch_filter(out.unit, &filter_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set glitch filter: %s", esp_err_to_name(ret));
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }
#endif

    // Create Channel A: edge on pin_a, level from pin_b
    pcnt_chan_config_t chan_a_cfg = {};
    chan_a_cfg.edge_gpio_num = pin_a;
    chan_a_cfg.level_gpio_num = pin_b;

    ret = pcnt_new_channel(out.unit, &chan_a_cfg, &out.channel_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create channel A (edge=%d level=%d): %s",
                 pin_a, pin_b, esp_err_to_name(ret));
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = pcnt_channel_set_edge_action(out.channel_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel A edge action: %s", esp_err_to_name(ret));
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = pcnt_channel_set_level_action(out.channel_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel A level action: %s", esp_err_to_name(ret));
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    // Create Channel B: edge on pin_b, level from pin_a
    pcnt_chan_config_t chan_b_cfg = {};
    chan_b_cfg.edge_gpio_num = pin_b;
    chan_b_cfg.level_gpio_num = pin_a;

    ret = pcnt_new_channel(out.unit, &chan_b_cfg, &out.channel_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create channel B (edge=%d level=%d): %s",
                 pin_b, pin_a, esp_err_to_name(ret));
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = pcnt_channel_set_edge_action(out.channel_b,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel B edge action: %s", esp_err_to_name(ret));
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = pcnt_channel_set_level_action(out.channel_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel B level action: %s", esp_err_to_name(ret));
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    // Add watch points for accumulator overflow tracking
    ret = pcnt_unit_add_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add high watch point: %s", esp_err_to_name(ret));
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = pcnt_unit_add_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add low watch point: %s", esp_err_to_name(ret));
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT);
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    // Enable unit (INIT -> ENABLED)
    ret = pcnt_unit_enable(out.unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable PCNT unit: %s", esp_err_to_name(ret));
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT);
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT);
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    // Clear count to start from zero
    ret = pcnt_unit_clear_count(out.unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear PCNT count: %s", esp_err_to_name(ret));
        pcnt_unit_disable(out.unit);
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT);
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT);
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    // Start unit (ENABLED -> RUNNING)
    ret = pcnt_unit_start(out.unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PCNT unit: %s", esp_err_to_name(ret));
        pcnt_unit_disable(out.unit);
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT);
        pcnt_unit_remove_watch_point(out.unit, CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT);
        pcnt_del_channel(out.channel_b);
        pcnt_del_channel(out.channel_a);
        pcnt_del_unit(out.unit);
        xSemaphoreGive(mtx);
        return ret;
    }

    s_active_count++;
    ESP_LOGI(TAG, "Allocated encoder: pin_a=%d pin_b=%d (%d/%d units in use)",
             pin_a, pin_b, s_active_count, kMaxUnits);

    xSemaphoreGive(mtx);
    return ESP_OK;
}

esp_err_t PcntPool::deallocate(const PcntUnit &unit)
{
    const SemaphoreHandle_t mtx = poolMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);

    pcnt_unit_stop(unit.unit);
    pcnt_unit_disable(unit.unit);
    pcnt_unit_remove_watch_point(unit.unit, CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT);
    pcnt_unit_remove_watch_point(unit.unit, CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT);
    pcnt_del_channel(unit.channel_b);
    pcnt_del_channel(unit.channel_a);
    pcnt_del_unit(unit.unit);

    s_active_count--;
    ESP_LOGI(TAG, "Deallocated encoder (%d/%d units in use)",
             s_active_count, kMaxUnits);

    xSemaphoreGive(mtx);
    return ESP_OK;
}

} // namespace detail
} // namespace esp_rotary_encoder
