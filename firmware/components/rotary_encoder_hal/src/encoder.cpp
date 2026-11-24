#include "encoder.hpp"
#include "pcnt_pool.hpp"
#include "esp_log.h"
#include "driver/pulse_cnt.h"
#include <utility>

static const char *const TAG = "encoder";

namespace rotary_encoder_hal {
namespace detail {

struct EncoderImpl {
    PcntUnit pcnt;
    esp_err_t init_error;
    bool valid;
};

} // namespace detail

Encoder::Encoder(gpio_num_t pin_a, gpio_num_t pin_b, uint32_t glitch_ns)
    : Encoder(pin_a, pin_b, glitch_ns, Options{})
{
}

Encoder::Encoder(gpio_num_t pin_a, gpio_num_t pin_b, uint32_t glitch_ns, const Options &options)
    : m_impl(new detail::EncoderImpl{})
{
    m_impl->valid = false;
    m_impl->init_error = ESP_ERR_INVALID_ARG;

    if (!GPIO_IS_VALID_GPIO(pin_a) || !GPIO_IS_VALID_GPIO(pin_b) || pin_a == pin_b) {
        ESP_LOGE(TAG, "Invalid encoder pins: pin_a=%d pin_b=%d (must be distinct valid GPIOs)", pin_a, pin_b);
        return;
    }

    // Allocate PCNT unit with quadrature decoding
    esp_err_t ret = detail::PcntPool::allocate(pin_a, pin_b, glitch_ns, m_impl->pcnt);
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

bool Encoder::isValid() const
{
    return m_impl && m_impl->valid;
}

esp_err_t Encoder::initError() const
{
    return m_impl ? m_impl->init_error : ESP_ERR_INVALID_STATE;
}

} // namespace rotary_encoder_hal
