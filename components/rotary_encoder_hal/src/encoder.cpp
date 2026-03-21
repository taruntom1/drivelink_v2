#include "encoder.hpp"
#include "pcnt_pool.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <utility>

static const char *TAG = "encoder";

namespace esp_rotary_encoder {

struct Encoder::Impl {
    detail::PcntUnit pcnt;
    std::atomic<int32_t> prev_count;
    std::atomic<int64_t> prev_time_us;
    std::atomic<float> current_rate;
    std::atomic<int32_t> last_delta;
    std::atomic<bool> sampling_in_progress;
    bool valid;
};

static bool acquire_sampling_lock(Encoder::Impl *impl)
{
    if (!impl || !impl->valid) {
        return false;
    }

    bool expected = false;
    constexpr int MAX_LOCK_ATTEMPTS = 8;
    for (int attempt = 0; attempt < MAX_LOCK_ATTEMPTS; ++attempt) {
        if (impl->sampling_in_progress.compare_exchange_weak(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
        expected = false;
        taskYIELD();
    }
    return false;
}

static void release_sampling_lock(Encoder::Impl *impl)
{
    impl->sampling_in_progress.store(false, std::memory_order_release);
}

static bool update_rate_sample(Encoder::Impl *impl, int32_t *sampled_count = nullptr)
{
    if (!acquire_sampling_lock(impl)) {
        return false;
    }

    int count = 0;
    esp_err_t ret = pcnt_unit_get_count(impl->pcnt.unit, &count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read PCNT count: %s", esp_err_to_name(ret));
        release_sampling_lock(impl);
        return false;
    }
    int64_t now_us = esp_timer_get_time();

    int32_t previous_count = impl->prev_count.load(std::memory_order_relaxed);
    int64_t previous_time_us = impl->prev_time_us.load(std::memory_order_relaxed);
    int32_t delta = static_cast<int32_t>(count) - previous_count;
    int64_t dt_us = now_us - previous_time_us;

    float rate = 0.0f;
    if (dt_us > 0) {
        rate = static_cast<float>(delta) / (static_cast<float>(dt_us) / 1000000.0f);
    }

    impl->current_rate.store(rate, std::memory_order_relaxed);
    impl->last_delta.store(delta, std::memory_order_relaxed);
    impl->prev_count.store(static_cast<int32_t>(count), std::memory_order_relaxed);
    impl->prev_time_us.store(now_us, std::memory_order_relaxed);
    if (sampled_count) {
        *sampled_count = static_cast<int32_t>(count);
    }
    release_sampling_lock(impl);
    return true;
}

Encoder::Encoder(gpio_num_t pin_a, gpio_num_t pin_b)
    : m_impl(new Impl{})
{
    m_impl->prev_count.store(0, std::memory_order_relaxed);
    m_impl->prev_time_us.store(0, std::memory_order_relaxed);
    m_impl->current_rate.store(0.0f, std::memory_order_relaxed);
    m_impl->last_delta.store(0, std::memory_order_relaxed);
    m_impl->sampling_in_progress.store(false, std::memory_order_relaxed);
    m_impl->valid = false;

    // Allocate PCNT unit with quadrature decoding
    esp_err_t ret = detail::PcntPool::allocate(pin_a, pin_b, m_impl->pcnt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate PCNT unit for pins %d/%d: %s",
                 pin_a, pin_b, esp_err_to_name(ret));
        return;
    }

    int count = 0;
    ret = pcnt_unit_get_count(m_impl->pcnt.unit, &count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read initial PCNT count for pins %d/%d: %s",
                 pin_a, pin_b, esp_err_to_name(ret));
        detail::PcntPool::deallocate(m_impl->pcnt);
        return;
    }

    m_impl->prev_count.store(static_cast<int32_t>(count), std::memory_order_relaxed);
    m_impl->prev_time_us.store(esp_timer_get_time(), std::memory_order_relaxed);

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
    int32_t sampled_count = 0;
    if (update_rate_sample(m_impl, &sampled_count)) {
        return sampled_count;
    }
    return m_impl->prev_count.load(std::memory_order_relaxed);
}

Direction Encoder::getDirection() const
{
    if (!m_impl || !m_impl->valid) {
        return Direction::Stopped;
    }
    update_rate_sample(m_impl);
    int32_t delta = m_impl->last_delta.load(std::memory_order_relaxed);

    if (delta > 0) return Direction::Forward;
    if (delta < 0) return Direction::Reverse;
    return Direction::Stopped;
}

float Encoder::getRate() const
{
    if (!m_impl || !m_impl->valid) {
        return 0.0f;
    }
    update_rate_sample(m_impl);
    return m_impl->current_rate.load(std::memory_order_relaxed);
}

void Encoder::reset()
{
    if (!m_impl || !m_impl->valid) {
        return;
    }

    if (!acquire_sampling_lock(m_impl)) {
        return;
    }

    esp_err_t ret = pcnt_unit_clear_count(m_impl->pcnt.unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear PCNT count: %s", esp_err_to_name(ret));
        release_sampling_lock(m_impl);
        return;
    }
    m_impl->prev_count.store(0, std::memory_order_relaxed);
    m_impl->prev_time_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    m_impl->current_rate.store(0.0f, std::memory_order_relaxed);
    m_impl->last_delta.store(0, std::memory_order_relaxed);
    release_sampling_lock(m_impl);
}

bool Encoder::isValid() const
{
    return m_impl && m_impl->valid;
}

} // namespace esp_rotary_encoder
