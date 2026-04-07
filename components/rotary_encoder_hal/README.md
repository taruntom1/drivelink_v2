# rotary_encoder_hal

## Overview
`rotary_encoder_hal` provides a quadrature encoder HAL built on ESP32-S3 PCNT. It abstracts PCNT unit setup/teardown and exposes position, direction, and rate APIs for motor feedback.

## Features
- One encoder per PCNT unit (up to 4 active encoders)
- 4x quadrature decoding
- Glitch filter support
- On-demand rate and direction sampling
- Resettable count/rate baseline

## Dependencies
Required:
- `esp_driver_pcnt`
- `esp_driver_gpio`
- `esp_timer`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_ENCODER_HAL_GLITCH_FILTER_NS` | int | 1000 | PCNT glitch reject width (ns), 0 disables. |
| `CONFIG_ENCODER_HAL_RATE_SAMPLE_PERIOD_MS` | int | 50 | Minimum interval between rate updates. |
| `CONFIG_ENCODER_HAL_PCNT_HIGH_LIMIT` | int | 32767 | PCNT high watch-point/wrap limit. |
| `CONFIG_ENCODER_HAL_PCNT_LOW_LIMIT` | int | -32768 | PCNT low watch-point/wrap limit. |

## Public API
Public header: `include/encoder.hpp`

- `Encoder(gpio_num_t pin_a, gpio_num_t pin_b)`
- `~Encoder()`
- `int32_t getCount() const`
- `Direction getDirection() const`
- `float getRate() const`
- `void reset()`
- `bool isValid() const`

## Usage Example
```cpp
#include "encoder.hpp"

esp_rotary_encoder::Encoder enc(GPIO_NUM_4, GPIO_NUM_5);
if (enc.isValid()) {
    int32_t count = enc.getCount();
    float rate = enc.getRate();
    (void)count;
    (void)rate;
}
```

## Error Handling
- Allocation/config/start failures result in `isValid()==false` and logged ESP-IDF errors.

## Thread Safety
- Shared PCNT pool operations are mutex-protected.
- Per-instance rate/direction state uses spinlock for internal consistency.

## Limitations & Known Issues
- Maximum 4 concurrent instances (ESP32-S3 PCNT units).
