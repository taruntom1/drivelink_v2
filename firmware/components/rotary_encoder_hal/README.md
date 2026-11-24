# rotary_encoder_hal

A move-only quadrature encoder counter on the ESP32-S3 PCNT. It only counts; `feedback.quadrature` ([mod_motor](../mod_motor/README.md)) turns counts into position and velocity with the [velocity estimator](../mod_motor/include/mod_motor/velocity_estimator.hpp), sampled at the loop rate.

## API ([`encoder.hpp`](include/encoder.hpp))

| Call | Contract |
|---|---|
| `Encoder(pin_a, pin_b[, {.pullup}])` | Takes one PCNT unit (both channels, 4x decoding) from the pool and applies the glitch filter. `pullup` (default true) keeps the driver's internal pull-ups on A and B; false disables them for push-pull encoders or external pull-ups. Setup only: the pool is mutex-guarded. |
| `isValid()`, `initError()` | `initError()` returns `ESP_OK` when valid, `ESP_ERR_INVALID_ARG` for bad pins, `ESP_ERR_NOT_FOUND` when the pool is full, `ESP_ERR_INVALID_STATE` for a moved-from object, or the driver error. |
| `readCount(count)` | The signed PCNT accumulator, extended past the 16-bit hardware counter by overflow watch points; it is reported as int32, so difference counts in unsigned arithmetic to keep a wrap well-defined. An ISR-safe driver call under the unit's spinlock with no allocation or log. Returns `ESP_ERR_INVALID_STATE` when not valid, or the driver error. IDF 5.5 compensates an overflow interrupt still pending at a read. |
| Destructor | Returns the unit to the pool. |

## Resources

The ESP32-S3 has four PCNT units, so at most four encoders run at once. A FreeRTOS mutex guards the pool, and limits are enforced in the allocation path.

## Kconfig

"Rotary Encoder HAL Configuration" in [Kconfig](Kconfig) sets `ENCODER_HAL_GLITCH_FILTER_NS` (default 1000 ns; 0 disables it) and the PCNT high and low limits (default the full 16-bit range, so overflow interrupts are rare).

## Rules

- It returns `esp_err_t` or `bool` and never reports through `diag`; the calling module reports.
- It has not run on a real encoder yet; see [Deferred hardware checks](../../../plans/host-status.md#deferred-hardware-checks).
