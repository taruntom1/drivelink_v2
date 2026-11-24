#include "esp_motor.hpp"
#include "mcpwm_pool.hpp"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include <algorithm>
#include <cmath>
#include <utility>

static const char *const TAG = "esp_motor";

namespace motor_hal {

struct Motor::Impl {
    detail::McpwmChannel channel;
    gpio_num_t dir_gpio;
    esp_err_t init_error;
    bool valid;
};

Motor::Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio, uint32_t pwm_freq_hz)
    : Motor(pwm_gpio, dir_gpio, pwm_freq_hz, Options{})
{
}

Motor::Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio, uint32_t pwm_freq_hz, const Options &options)
    : m_impl(new Impl{})
{
    m_impl->dir_gpio = dir_gpio;
    m_impl->valid = false;
    m_impl->init_error = ESP_ERR_INVALID_ARG;

    if (!GPIO_IS_VALID_OUTPUT_GPIO(pwm_gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(dir_gpio) || pwm_gpio == dir_gpio) {
        ESP_LOGE(TAG, "Invalid motor pins: pwm_gpio=%d dir_gpio=%d (must be distinct output-capable GPIOs)",
                 pwm_gpio, dir_gpio);
        return;
    }

    // Configure direction GPIO as output
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << dir_gpio;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure direction GPIO %d: %s",
                 dir_gpio, esp_err_to_name(ret));
        m_impl->init_error = ret;
        return;
    }
    gpio_set_level(dir_gpio, 0);

    // Allocate MCPWM channel
    ret = detail::McpwmPool::allocate(static_cast<int>(pwm_gpio), pwm_freq_hz, m_impl->channel,
                                      options.start_forced);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate MCPWM channel for GPIO %d: %s",
                 pwm_gpio, esp_err_to_name(ret));
        gpio_reset_pin(dir_gpio); // leave the object consistent: release DIR
        m_impl->init_error = ret;
        return;
    }

    m_impl->init_error = ESP_OK;
    m_impl->valid = true;
    ESP_LOGI(TAG, "Motor created: pwm_gpio=%d dir_gpio=%d", pwm_gpio, dir_gpio);
}

Motor::~Motor()
{
    if (m_impl) {
        if (m_impl->valid) {
            (void)write(0.0f);
            detail::McpwmPool::deallocate(m_impl->channel);
        }
        delete m_impl;
        m_impl = nullptr;
    }
}

Motor::Motor(Motor &&other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr))
{
}

Motor &Motor::operator=(Motor &&other) noexcept
{
    if (this != &other) {
        if (m_impl) {
            if (m_impl->valid) {
                (void)write(0.0f);
                detail::McpwmPool::deallocate(m_impl->channel);
            }
            delete m_impl;
        }
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

esp_err_t Motor::write(float speed)
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!std::isfinite(speed)) {
        speed = 0.0f; // NaN would survive the clamp and break the compare value
    }
    speed = std::clamp(speed, -1.0f, 1.0f);

    float magnitude = std::fabsf(speed);
    uint32_t compare_value = static_cast<uint32_t>(
        magnitude * static_cast<float>(m_impl->channel.period_ticks));

    if (compare_value > m_impl->channel.period_ticks) {
        compare_value = m_impl->channel.period_ticks;
    }

    // DIR changes at once, the compare value at the next PWM period. A zero
    // write leaves DIR alone, so DIR never flips under a live compare value
    // of the other sign when callers pass through zero on a reversal.
    esp_err_t ret = ESP_OK;
    if (compare_value > 0) {
        ret = gpio_set_level(m_impl->dir_gpio, speed < 0.0f ? 1 : 0);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ret = mcpwm_comparator_set_compare_value(m_impl->channel.comparator, compare_value);
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_OK;
}

esp_err_t Motor::forceLow()
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }
    return mcpwm_generator_set_force_level(m_impl->channel.generator, 0, true);
}

esp_err_t Motor::releaseForce()
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }
    return mcpwm_generator_set_force_level(m_impl->channel.generator, -1, true);
}

bool Motor::isValid() const
{
    return m_impl && m_impl->valid;
}

esp_err_t Motor::initError() const
{
    return m_impl ? m_impl->init_error : ESP_ERR_INVALID_STATE;
}

} // namespace motor_hal
