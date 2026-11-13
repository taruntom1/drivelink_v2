#include "mcpwm_pool.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/mcpwm_prelude.h"

static const char *const TAG = "mcpwm_pool";

namespace motor_hal {
namespace detail {

McpwmPool::GroupState McpwmPool::s_groups[kNumGroups] = {};

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

bool McpwmPool::findFreeSlot(bool exclusive, int &group_id, int &oper_index, int &slot)
{
    for (int g = 0; g < kNumGroups; g++) {
        for (int o = 0; o < kOpersPerGroup; o++) {
            const OperatorState &op = s_groups[g].opers[o];
            const bool free = exclusive ? op.user_count == 0
                                        : (!op.exclusive && op.user_count < kSlotsPerOper);
            if (free) {
                group_id = g;
                oper_index = o;
                slot = s_groups[g].opers[o].user_count;
                return true;
            }
        }
    }
    return false;
}

esp_err_t McpwmPool::ensureTimer(int group_id)
{
    GroupState &g = s_groups[group_id];
    if (g.timer != nullptr) {
        return ESP_OK;
    }

    uint32_t period = CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ / CONFIG_MOTOR_HAL_PWM_FREQ_HZ;

    mcpwm_timer_config_t timer_cfg = {};
    timer_cfg.group_id = group_id;
    timer_cfg.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    timer_cfg.resolution_hz = CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ;
    timer_cfg.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    timer_cfg.period_ticks = period;

    esp_err_t ret = mcpwm_new_timer(&timer_cfg, &g.timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer for group %d: %s", group_id, esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_timer_enable(g.timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer for group %d: %s", group_id, esp_err_to_name(ret));
        mcpwm_del_timer(g.timer);
        g.timer = nullptr;
        return ret;
    }

    ret = mcpwm_timer_start_stop(g.timer, MCPWM_TIMER_START_NO_STOP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer for group %d: %s", group_id, esp_err_to_name(ret));
        mcpwm_timer_disable(g.timer);
        mcpwm_del_timer(g.timer);
        g.timer = nullptr;
        return ret;
    }

    g.timer_enabled = true;
    ESP_LOGI(TAG, "Created timer: group=%d resolution=%d Hz period=%lu ticks",
             group_id, CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ, (unsigned long)period);
    return ESP_OK;
}

esp_err_t McpwmPool::ensureOperator(int group_id, int oper_index)
{
    GroupState &g = s_groups[group_id];
    OperatorState &op = g.opers[oper_index];

    if (op.handle != nullptr) {
        return ESP_OK;
    }

    mcpwm_operator_config_t oper_cfg = {};
    oper_cfg.group_id = group_id;

    esp_err_t ret = mcpwm_new_operator(&oper_cfg, &op.handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create operator: group=%d idx=%d: %s",
                 group_id, oper_index, esp_err_to_name(ret));
        return ret;
    }

    ret = mcpwm_operator_connect_timer(op.handle, g.timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect operator to timer: %s", esp_err_to_name(ret));
        mcpwm_del_operator(op.handle);
        op.handle = nullptr;
        op.exclusive = false;
        return ret;
    }

    ESP_LOGI(TAG, "Created operator: group=%d idx=%d", group_id, oper_index);
    return ESP_OK;
}

esp_err_t McpwmPool::allocate(int pwm_gpio, McpwmChannel &out, bool exclusive_operator, bool start_forced)
{
    const SemaphoreHandle_t mtx = poolMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);

    int gid, oidx, slot;
    if (!findFreeSlot(exclusive_operator, gid, oidx, slot)) {
        xSemaphoreGive(mtx);
        ESP_LOGE(TAG, "No free MCPWM %s (max %d motors, %d with exclusive operators)",
                 exclusive_operator ? "operator" : "slot",
                 kNumGroups * kOpersPerGroup * kSlotsPerOper, kNumGroups * kOpersPerGroup);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ensureTimer(gid);
    if (ret != ESP_OK) {
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = ensureOperator(gid, oidx);
    if (ret != ESP_OK) {
        xSemaphoreGive(mtx);
        return ret;
    }

    // Create comparator
    mcpwm_comparator_config_t cmp_cfg = {};
    cmp_cfg.flags.update_cmp_on_tez = true;

    mcpwm_cmpr_handle_t cmpr = nullptr;
    ret = mcpwm_new_comparator(s_groups[gid].opers[oidx].handle, &cmp_cfg, &cmpr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create comparator: %s", esp_err_to_name(ret));
        xSemaphoreGive(mtx);
        return ret;
    }

    mcpwm_comparator_set_compare_value(cmpr, 0);

    // Create generator
    mcpwm_generator_config_t gen_cfg = {};
    gen_cfg.gen_gpio_num = pwm_gpio;

    mcpwm_gen_handle_t gen = nullptr;
    ret = mcpwm_new_generator(s_groups[gid].opers[oidx].handle, &gen_cfg, &gen);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create generator: %s", esp_err_to_name(ret));
        mcpwm_del_comparator(cmpr);
        xSemaphoreGive(mtx);
        return ret;
    }

    if (start_forced) {
        ret = mcpwm_generator_set_force_level(gen, 0, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to force generator low: %s", esp_err_to_name(ret));
            mcpwm_del_generator(gen);
            mcpwm_del_comparator(cmpr);
            xSemaphoreGive(mtx);
            return ret;
        }
    }

    // Set PWM actions: HIGH on timer empty, LOW on comparator match
    ret = mcpwm_generator_set_action_on_timer_event(gen,
              MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                           MCPWM_TIMER_EVENT_EMPTY,
                                           MCPWM_GEN_ACTION_HIGH));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer action: %s", esp_err_to_name(ret));
        mcpwm_del_generator(gen);
        mcpwm_del_comparator(cmpr);
        xSemaphoreGive(mtx);
        return ret;
    }

    ret = mcpwm_generator_set_action_on_compare_event(gen,
              MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                             cmpr,
                                             MCPWM_GEN_ACTION_LOW));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set compare action: %s", esp_err_to_name(ret));
        mcpwm_del_generator(gen);
        mcpwm_del_comparator(cmpr);
        xSemaphoreGive(mtx);
        return ret;
    }

    // Update bookkeeping
    uint32_t period = CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ / CONFIG_MOTOR_HAL_PWM_FREQ_HZ;
    s_groups[gid].opers[oidx].user_count++;
    s_groups[gid].opers[oidx].exclusive = exclusive_operator;
    s_groups[gid].total_users++;

    out.group_id = gid;
    out.oper_index = oidx;
    out.slot = slot;
    out.timer = s_groups[gid].timer;
    out.oper = s_groups[gid].opers[oidx].handle;
    out.comparator = cmpr;
    out.generator = gen;
    out.period_ticks = period;

    ESP_LOGI(TAG, "Allocated motor: group=%d oper=%d slot=%d gpio=%d",
             gid, oidx, slot, pwm_gpio);

    xSemaphoreGive(mtx);
    return ESP_OK;
}

esp_err_t McpwmPool::deallocate(const McpwmChannel &ch)
{
    const SemaphoreHandle_t mtx = poolMutex();
    xSemaphoreTake(mtx, portMAX_DELAY);

    // Force generator output low before deleting
    mcpwm_generator_set_force_level(ch.generator, 0, true);

    mcpwm_del_generator(ch.generator);
    mcpwm_del_comparator(ch.comparator);

    GroupState &g = s_groups[ch.group_id];
    OperatorState &op = g.opers[ch.oper_index];
    op.user_count--;

    if (op.user_count == 0) {
        mcpwm_del_operator(op.handle);
        op.handle = nullptr;
        op.exclusive = false;
        ESP_LOGI(TAG, "Deleted operator: group=%d idx=%d", ch.group_id, ch.oper_index);
    }

    g.total_users--;

    if (g.total_users == 0 && g.timer != nullptr) {
        mcpwm_timer_start_stop(g.timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(g.timer);
        mcpwm_del_timer(g.timer);
        g.timer = nullptr;
        g.timer_enabled = false;
        ESP_LOGI(TAG, "Deleted timer for group %d", ch.group_id);
    }

    ESP_LOGI(TAG, "Deallocated motor: group=%d oper=%d slot=%d",
             ch.group_id, ch.oper_index, ch.slot);

    xSemaphoreGive(mtx);
    return ESP_OK;
}

} // namespace detail
} // namespace motor_hal
