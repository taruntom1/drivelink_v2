#include "encoder.hpp"
#include "pcnt_pool.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/pulse_cnt.h"
#include <utility>

static const char *const TAG = "encoder";

namespace rotary_encoder_hal {
namespace detail {

struct EncoderImpl {
    PcntUnit pcnt;
    portMUX_TYPE spinlock;
    int32_t prev_count;
    int64_t prev_time_us;
    float current_rate;
    int32_t last_delta;
    esp_err_t init_error;
    bool valid;
};

} // namespace detail

// PRECONDITION: Caller must hold impl->spinlock. Must not log: the caller
// is inside a critical section. It returns the error instead, and the caller
// logs it after taskEXIT_CRITICAL.
static esp_err_t update_rate_sample_locked(detail::EncoderImpl *impl)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t sample_period_us =
        static_cast<int64_t>(CONFIG_ENCODER_HAL_RATE_SAMPLE_PERIOD_MS) * 1000;
    if ((now_us - impl->prev_time_us) < sample_period_us) {
        return ESP_OK;
    }

    int count = 0;
    const esp_err_t ret = pcnt_unit_get_count(impl->pcnt.unit, &count);
    if (ret != ESP_OK) {
        return ret;
    }

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
    return ESP_OK;
}

static void log_rate_sample_error(esp_err_t ret)
{
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read PCNT count: %s", esp_err_to_name(ret));
    }
}

Encoder::Encoder(gpio_num_t pin_a, gpio_num_t pin_b)
    : Encoder(pin_a, pin_b, Options{})
{
}

Encoder::Encoder(gpio_num_t pin_a, gpio_num_t pin_b, const Options &options)
    : m_impl(new detail::EncoderImpl{})
{
    m_impl->spinlock = portMUX_INITIALIZER_UNLOCKED;
    m_impl->prev_count = 0;
    m_impl->prev_time_us = esp_timer_get_time();
    m_impl->current_rate = 0.0f;
    m_impl->last_delta = 0;
    m_impl->valid = false;
    m_impl->init_error = ESP_ERR_INVALID_ARG;

    if (!GPIO_IS_VALID_GPIO(pin_a) || !GPIO_IS_VALID_GPIO(pin_b) || pin_a == pin_b) {
        ESP_LOGE(TAG, "Invalid encoder pins: pin_a=%d pin_b=%d (must be distinct valid GPIOs)", pin_a, pin_b);
        return;
    }

    // Allocate PCNT unit with quadrature decoding
    esp_err_t ret = detail::PcntPool::allocate(pin_a, pin_b, m_impl->pcnt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate PCNT unit for pins %d/%d: %s",
                 pin_a, pin_b, esp_err_to_name(ret));
        m_impl->init_error = ret;
        return;
    }

    if (!options.pullup) {
        ret = gpio_pullup_dis(pin_a);
        if (ret == ESP_OK) {
            ret = gpio_pullup_dis(pin_b);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to disable pull-ups on pins %d/%d: %s",
                     pin_a, pin_b, esp_err_to_name(ret));
            detail::PcntPool::deallocate(m_impl->pcnt);
            m_impl->init_error = ret;
            return;
        }
    }

    int count = 0;
    ret = pcnt_unit_get_count(m_impl->pcnt.unit, &count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read initial PCNT count for pins %d/%d: %s",
                 pin_a, pin_b, esp_err_to_name(ret));
        detail::PcntPool::deallocate(m_impl->pcnt);
        m_impl->init_error = ret;
        return;
    }
    m_impl->prev_count = static_cast<int32_t>(count);

    m_impl->init_error = ESP_OK;
    m_impl->valid = true;
    ESP_LOGI(TAG, "Encoder created: pin_a=%d pin_b=%d", pin_a, pin_b);
}

Encoder::~Encoder()
{
    if (m_impl) {
        if (m_impl->valid) {
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

esp_err_t Encoder::readCount(int32_t &count) const
{
    if (!m_impl || !m_impl->valid) {
        return ESP_ERR_INVALID_STATE;
    }
    int value = 0;
    const esp_err_t ret = pcnt_unit_get_count(m_impl->pcnt.unit, &value);
    if (ret == ESP_OK) {
        count = static_cast<int32_t>(value);
    }
    return ret;
}

Direction Encoder::getDirection() const
{
    if (!m_impl || !m_impl->valid) {
        return Direction::Stopped;
    }
    int32_t delta;
    taskENTER_CRITICAL(&m_impl->spinlock);
    const esp_err_t ret = update_rate_sample_locked(m_impl);
    delta = m_impl->last_delta;
    taskEXIT_CRITICAL(&m_impl->spinlock);
    log_rate_sample_error(ret);

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
    const esp_err_t ret = update_rate_sample_locked(m_impl);
    rate = m_impl->current_rate;
    taskEXIT_CRITICAL(&m_impl->spinlock);
    log_rate_sample_error(ret);
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

esp_err_t Encoder::initError() const
{
    return m_impl ? m_impl->init_error : ESP_ERR_INVALID_STATE;
}

} // namespace rotary_encoder_hal
