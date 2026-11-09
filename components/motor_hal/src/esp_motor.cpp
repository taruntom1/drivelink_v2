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
    float current_speed;
    esp_err_t init_error;
    bool valid;
};

Motor::Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio)
    : m_impl(new Impl{})
{
    m_impl->dir_gpio = dir_gpio;
    m_impl->current_speed = 0.0f;
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
    ret = detail::McpwmPool::allocate(static_cast<int>(pwm_gpio), m_impl->channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate MCPWM channel for GPIO %d: %s",
                 pwm_gpio, esp_err_to_name(ret));
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
            stop();
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
                stop();
                detail::McpwmPool::deallocate(m_impl->channel);
            }
            delete m_impl;
        }
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

void Motor::setSpeed(float speed)
{
    if (!m_impl || !m_impl->valid) {
        return;
    }

    if (!std::isfinite(speed)) {
        speed = 0.0f; // NaN would survive the clamp and break the compare value
    }
    speed = std::clamp(speed, -1.0f, 1.0f);

    bool reverse = speed < 0.0f;
    gpio_set_level(m_impl->dir_gpio, reverse ? 1 : 0);

    float magnitude = std::fabsf(speed);
    uint32_t compare_value = static_cast<uint32_t>(
        magnitude * static_cast<float>(m_impl->channel.period_ticks));

    if (compare_value > m_impl->channel.period_ticks) {
        compare_value = m_impl->channel.period_ticks;
    }

    mcpwm_comparator_set_compare_value(m_impl->channel.comparator, compare_value);
    m_impl->current_speed = speed;
}

void Motor::setSpeedPercent(float percent)
{
    setSpeed(percent / 100.0f);
}

void Motor::stop()
{
    setSpeed(0.0f);
}

bool Motor::isValid() const
{
    return m_impl && m_impl->valid;
}

esp_err_t Motor::initError() const
{
    return m_impl ? m_impl->init_error : ESP_ERR_INVALID_STATE;
}

float Motor::getSpeed() const
{
    if (!m_impl || !m_impl->valid) {
        return 0.0f;
    }
    return m_impl->current_speed;
}

} // namespace motor_hal
