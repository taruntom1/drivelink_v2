#include "encoder.hpp"
#include "pcnt_pool.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/pulse_cnt.h"
#include <utility>

static const char *TAG = "encoder";

namespace esp_rotary_encoder {

struct Encoder::Impl {
    detail::PcntUnit pcnt;
    esp_timer_handle_t rate_timer;
    portMUX_TYPE spinlock;
    int32_t prev_count;
    int64_t prev_time_us;
    float current_rate;
    int32_t last_delta;
    bool valid;
};

static void rate_timer_callback(void *arg)
{
    auto *impl = static_cast<Encoder::Impl *>(arg);

    taskENTER_CRITICAL(&impl->spinlock);

    int count = 0;
    pcnt_unit_get_count(impl->pcnt.unit, &count);
    int64_t now_us = esp_timer_get_time();

    int32_t delta = static_cast<int32_t>(count) - impl->prev_count;
    int64_t dt_us = now_us - impl->prev_time_us;

    float rate = 0.0f;
    if (dt_us > 0) {
        rate = static_cast<float>(delta) / (static_cast<float>(dt_us) / 1000000.0f);
    }

    impl->current_rate = rate;
    impl->last_delta = delta;
    impl->prev_count = static_cast<int32_t>(count);
    impl->prev_time_us = now_us;

    taskEXIT_CRITICAL(&impl->spinlock);
}

Encoder::Encoder(gpio_num_t pin_a, gpio_num_t pin_b)
    : m_impl(new Impl{})
{
    m_impl->rate_timer = nullptr;
    m_impl->spinlock = portMUX_INITIALIZER_UNLOCKED;
    m_impl->prev_count = 0;
    m_impl->prev_time_us = 0;
    m_impl->current_rate = 0.0f;
    m_impl->last_delta = 0;
    m_impl->valid = false;

    // Allocate PCNT unit with quadrature decoding
    esp_err_t ret = detail::PcntPool::allocate(pin_a, pin_b, m_impl->pcnt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate PCNT unit for pins %d/%d: %s",
                 pin_a, pin_b, esp_err_to_name(ret));
        return;
    }

    // Create periodic timer for rate sampling
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = rate_timer_callback;
    timer_args.arg = m_impl;
    timer_args.name = "enc_rate";
    timer_args.dispatch_method = ESP_TIMER_TASK;

    ret = esp_timer_create(&timer_args, &m_impl->rate_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create rate timer: %s", esp_err_to_name(ret));
        detail::PcntPool::deallocate(m_impl->pcnt);
        return;
    }

    m_impl->prev_time_us = esp_timer_get_time();

    ret = esp_timer_start_periodic(m_impl->rate_timer,
        CONFIG_ENCODER_HAL_RATE_SAMPLE_PERIOD_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start rate timer: %s", esp_err_to_name(ret));
        esp_timer_delete(m_impl->rate_timer);
        detail::PcntPool::deallocate(m_impl->pcnt);
        return;
    }

    m_impl->valid = true;
    ESP_LOGI(TAG, "Encoder created: pin_a=%d pin_b=%d", pin_a, pin_b);
}

Encoder::~Encoder()
{
    if (m_impl) {
        if (m_impl->valid) {
            esp_timer_stop(m_impl->rate_timer);
            esp_timer_delete(m_impl->rate_timer);
            detail::PcntPool::deallocate(m_impl->pcnt);
        }
        delete m_impl;
        m_impl = nullptr;
    }
}

Encoder::Encoder(Encoder &&other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr))
{
}

Encoder &Encoder::operator=(Encoder &&other) noexcept
{
    if (this != &other) {
        if (m_impl) {
            if (m_impl->valid) {
                esp_timer_stop(m_impl->rate_timer);
                esp_timer_delete(m_impl->rate_timer);
                detail::PcntPool::deallocate(m_impl->pcnt);
            }
            delete m_impl;
        }
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

int32_t Encoder::getCount() const
{
    if (!m_impl || !m_impl->valid) {
        return 0;
    }
    int count = 0;
    pcnt_unit_get_count(m_impl->pcnt.unit, &count);
    return static_cast<int32_t>(count);
}

Direction Encoder::getDirection() const
{
    if (!m_impl || !m_impl->valid) {
        return Direction::Stopped;
    }
    int32_t delta;
    taskENTER_CRITICAL(&m_impl->spinlock);
    delta = m_impl->last_delta;
    taskEXIT_CRITICAL(&m_impl->spinlock);

    if (delta > 0) return Direction::Forward;
    if (delta < 0) return Direction::Reverse;
    return Direction::Stopped;
}

float Encoder::getRate() const
{
    if (!m_impl || !m_impl->valid) {
        return 0.0f;
    }
    float rate;
    taskENTER_CRITICAL(&m_impl->spinlock);
    rate = m_impl->current_rate;
    taskEXIT_CRITICAL(&m_impl->spinlock);
    return rate;
}

void Encoder::reset()
{
    if (!m_impl || !m_impl->valid) {
        return;
    }
    taskENTER_CRITICAL(&m_impl->spinlock);
    pcnt_unit_clear_count(m_impl->pcnt.unit);
    m_impl->prev_count = 0;
    m_impl->prev_time_us = esp_timer_get_time();
    m_impl->current_rate = 0.0f;
    m_impl->last_delta = 0;
    taskEXIT_CRITICAL(&m_impl->spinlock);
}

bool Encoder::isValid() const
{
    return m_impl && m_impl->valid;
}

} // namespace esp_rotary_encoder
