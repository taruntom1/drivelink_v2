#include "mcpwm_pool.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/mcpwm_prelude.h"

static const char *const TAG = "mcpwm_pool";

namespace motor_hal {
namespace detail {

int McpwmPool::s_users[kNumGroups] = {};

SemaphoreHandle_t McpwmPool::poolMutex()
{
    // Function-local static: C++ guarantees one-time, thread-safe
    // initialisation (ESP-IDF implements the guard with FreeRTOS), so two
    // tasks creating their first motor at once can't create two mutexes.
    static const SemaphoreHandle_t mutex = [] {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        assert(m != nullptr);
        return m;
    }();
    return mutex;
}

esp_err_t McpwmPool::createTimer(McpwmChannel &ch)
{
    mcpwm_timer_config_t timer_cfg = {};
    timer_cfg.group_id = ch.group_id;
    timer_cfg.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    timer_cfg.resolution_hz = CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ;
    timer_cfg.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    timer_cfg.period_ticks = ch.period_ticks;

    mcpwm_timer_handle_t timer = nullptr;
    esp_err_t ret = mcpwm_new_timer(&timer_cfg, &timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer in group %d: %s", ch.group_id, esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_timer_enable(timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer in group %d: %s", ch.group_id, esp_err_to_name(ret));
        mcpwm_del_timer(timer);
        return ret;
    }

    ret = mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer in group %d: %s", ch.group_id, esp_err_to_name(ret));
        mcpwm_timer_disable(timer);
        mcpwm_del_timer(timer);
        return ret;
    }

    ch.timer = timer;
    return ESP_OK;
}

esp_err_t McpwmPool::create(int pwm_gpio, bool start_forced, McpwmChannel &ch)
{
    esp_err_t ret = createTimer(ch);
    if (ret != ESP_OK) {
        return ret;
    }

    mcpwm_operator_config_t oper_cfg = {};
    oper_cfg.group_id = ch.group_id;
    ret = mcpwm_new_operator(&oper_cfg, &ch.oper);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create operator in group %d: %s", ch.group_id, esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_operator_connect_timer(ch.oper, ch.timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect operator to timer: %s", esp_err_to_name(ret));
        return ret;
    }

    mcpwm_comparator_config_t cmp_cfg = {};
    cmp_cfg.flags.update_cmp_on_tez = true;
    ret = mcpwm_new_comparator(ch.oper, &cmp_cfg, &ch.comparator);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create comparator: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_comparator_set_compare_value(ch.comparator, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set the compare value: %s", esp_err_to_name(ret));
        return ret;
    }

    mcpwm_generator_config_t gen_cfg = {};
    gen_cfg.gen_gpio_num = pwm_gpio;
    ret = mcpwm_new_generator(ch.oper, &gen_cfg, &ch.generator);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create generator: %s", esp_err_to_name(ret));
        return ret;
    }

    if (start_forced) {
        ret = mcpwm_generator_set_force_level(ch.generator, 0, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to force generator low: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Set PWM actions: HIGH on timer empty, LOW on comparator match
    ret = mcpwm_generator_set_action_on_timer_event(ch.generator,
              MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                           MCPWM_TIMER_EVENT_EMPTY,
                                           MCPWM_GEN_ACTION_HIGH));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer action: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_generator_set_action_on_compare_event(ch.generator,
              MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                             ch.comparator,
                                             MCPWM_GEN_ACTION_LOW));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set compare action: %s", esp_err_to_name(ret));
    }
    return ret;
}

void McpwmPool::destroy(McpwmChannel &ch)
{
    if (ch.generator != nullptr) {
        // Force the output low before the generator releases the pin
        mcpwm_generator_set_force_level(ch.generator, 0, true);
        mcpwm_del_generator(ch.generator);
    }
    if (ch.comparator != nullptr) {
        mcpwm_del_comparator(ch.comparator);
    }
    if (ch.oper != nullptr) {
        mcpwm_del_operator(ch.oper);
    }
    if (ch.timer != nullptr) {
        mcpwm_timer_start_stop(ch.timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(ch.timer);
        mcpwm_del_timer(ch.timer);
    }
    ch = {};
}

esp_err_t McpwmPool::allocate(int pwm_gpio, uint32_t pwm_freq_hz, McpwmChannel &out, bool start_forced)
{
    if (pwm_freq_hz == 0 || pwm_freq_hz > CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ) {
        ESP_LOGE(TAG, "PWM frequency %lu Hz is out of range", static_cast<unsigned long>(pwm_freq_hz));
        return ESP_ERR_INVALID_ARG;
    }

    const SemaphoreHandle_t mtx = poolMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);

    McpwmChannel ch;
    while (ch.group_id < kNumGroups && s_users[ch.group_id] == kMotorsPerGroup) {
        ++ch.group_id;
    }
    if (ch.group_id == kNumGroups) {
        xSemaphoreGive(mtx);
        ESP_LOGE(TAG, "No free MCPWM operator (max %d motors)", kNumGroups * kMotorsPerGroup);
        return ESP_ERR_NOT_FOUND;
    }

    ch.period_ticks = (CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ + pwm_freq_hz / 2) / pwm_freq_hz;
    const esp_err_t ret = create(pwm_gpio, start_forced, ch);
    if (ret == ESP_OK) {
        ++s_users[ch.group_id];
        out = ch;
        ESP_LOGI(TAG, "Allocated motor: group=%d gpio=%d %lu Hz, %lu duty steps", ch.group_id, pwm_gpio,
                 static_cast<unsigned long>(CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ / ch.period_ticks),
                 static_cast<unsigned long>(ch.period_ticks));
    } else {
        destroy(ch);
    }

    xSemaphoreGive(mtx);
    return ret;
}

void McpwmPool::deallocate(McpwmChannel &ch)
{
    const SemaphoreHandle_t mtx = poolMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);

    const int group_id = ch.group_id;
    destroy(ch);
    --s_users[group_id];
    ESP_LOGI(TAG, "Deallocated motor: group=%d", group_id);

    xSemaphoreGive(mtx);
}

} // namespace detail
} // namespace motor_hal
