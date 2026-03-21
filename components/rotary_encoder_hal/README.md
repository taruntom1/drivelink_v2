# rotary_encoder_hal Component

`rotary_encoder_hal` provides a C++ HAL for quadrature rotary encoders on ESP32-S3 using the PCNT peripheral.

Each `Encoder` instance:

- Allocates one PCNT unit from a shared pool
- Configures both PCNT channels for 4x quadrature decoding
- Optionally applies hardware glitch filtering
- Computes signed rate and direction on getter calls

This component is intended for wheel odometry, shaft position tracking, and closed-loop motor control feedback.

## What You Get

Public header: `components/rotary_encoder_hal/include/encoder.hpp`

- Position count (`getCount`) using PCNT accumulator support
- Signed rate in pulses/second (`getRate`)
- Direction from latest sample window (`getDirection`)
- Count/rate baseline reset (`reset`)
- Runtime validity check (`isValid`)

## Resource Model

- ESP32-S3 has 4 PCNT units
- Each `Encoder` consumes 1 PCNT unit
- Maximum active encoders at once: 4

If all units are in use, construction fails internally and `isValid()` returns `false`.

## API: When to Use What

### `getCount()`
Use for absolute/relative position and odometry integration.

- Returns accumulated pulse count (`int32_t`)
- Best for distance and angle tracking

Choose this when you need long-term position, not just instantaneous speed.

### `getRate()`
Use for speed feedback loops.

- Returns signed pulses/second (`float`)
- Computed on demand when called (throttled by `ENCODER_HAL_RATE_SAMPLE_PERIOD_MS`)

Choose this when your controller needs velocity.

### `getDirection()`
Use for simple motion state.

- Returns `Forward`, `Reverse`, or `Stopped`
- Derived from the most recent sample delta

Choose this when you only need direction and not exact speed.

### `reset()`
Use when starting a new measurement interval.

- Clears hardware count
- Resets rate/direction baseline to avoid stale deltas

Typical uses: homing complete, new motion segment, or test setup.

### `isValid()`
Call immediately after construction and before reads.

- `false` means allocation/config/start failed
- Common causes: PCNT exhaustion or peripheral setup failure

## Minimal Usage Example

```cpp
#include "encoder.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace esp_rotary_encoder;

extern "C" void app_main(void)
{
    Encoder wheel(GPIO_NUM_4, GPIO_NUM_5);

    if (!wheel.isValid()) {
        ESP_LOGE("app", "Encoder init failed");
        return;
    }

    while (true) {
        int32_t pos = wheel.getCount();
        float rate = wheel.getRate();
        Direction dir = wheel.getDirection();

        (void)pos;
        (void)rate;
        (void)dir;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

## Configuration (menuconfig)

Path:

- `Rotary Encoder HAL Configuration`

Key settings:

- `ENCODER_HAL_GLITCH_FILTER_NS`
  - Rejects pulses shorter than this duration
  - Set `0` to disable filtering
- `ENCODER_HAL_RATE_SAMPLE_PERIOD_MS`
  - Faster updates with lower value, smoother/noisier tradeoff
- `ENCODER_HAL_PCNT_HIGH_LIMIT`
- `ENCODER_HAL_PCNT_LOW_LIMIT`
  - Hardware counter wrap points used with accumulator support

Recommended starting values are already provided in Kconfig defaults.

## Integration Notes

- Component dependencies are already declared:
  - `esp_driver_pcnt`
  - `esp_driver_gpio`
  - `esp_timer`
- Any component using this API should add `REQUIRES rotary_encoder_hal`.

Typical setup flow:

1. `idf.py menuconfig`
2. `idf.py build`

## Behavior and Concurrency Notes

- Rate/direction are updated on `getRate()`/`getDirection()` calls.
- Sampling is throttled by `ENCODER_HAL_RATE_SAMPLE_PERIOD_MS` to keep a stable window.
- Internal state for rate/direction uses a spinlock for safe concurrent reads.
- `getCount()` reads the PCNT accumulator-backed count directly.
- Use one `Encoder` instance per physical quadrature sensor.

## Quick Decision Guide

- Need distance/angle: use `getCount()`
- Need wheel speed estimate: use `getRate()`
- Need movement sign only: use `getDirection()`
- Need to restart measurement reference: call `reset()`
