#pragma once

#include <cstdint>

namespace core::gpio_policy {

/// The bit of pin `gpio` in a 64-bit pin mask (0 for a pin above 63).
constexpr uint64_t gpioBit(unsigned gpio)
{
    return (gpio < 64U) ? (uint64_t{1} << gpio) : 0U;
}

/// The bits of pins `first` to `last`, both included.
constexpr uint64_t gpioRange(unsigned first, unsigned last)
{
    uint64_t mask = 0;
    for (unsigned g = first; g <= last; ++g) {
        mask |= gpioBit(g);
    }
    return mask;
}

/// ESP32-S3 GPIO numbers run 0..48.
inline constexpr unsigned kEsp32s3GpioCount = 49;

/// Numbers in 0..48 that have no GPIO on the ESP32-S3.
inline constexpr uint64_t kEsp32s3Nonexistent = gpioRange(22, 25);

/// Pins a module may never claim, whatever the board:
/// - 19, 20: USB D-/D+ (the console runs on USB-Serial-JTAG)
/// - 26..32: SPI flash / quad PSRAM
/// - 33..37: octal flash / PSRAM (reserved on every module, so a board change
///   cannot make them unsafe)
inline constexpr uint64_t kEsp32s3Reserved = gpioBit(19) | gpioBit(20) | gpioRange(26, 37);

/// The pins the ESP32-S3 policy allows: the serial link may use these.
inline constexpr uint64_t kEsp32s3Allowed =
    gpioRange(0, kEsp32s3GpioCount - 1U) & ~(kEsp32s3Nonexistent | kEsp32s3Reserved);

/// UART0's own pins, the factory link. The ROM prints on GPIO 43 at every
/// reset, which would drive whatever a module has there, so no module may claim
/// them even when the link has moved away.
inline constexpr uint64_t kEsp32s3Uart0 = gpioBit(43) | gpioBit(44);

/// The pins a module may claim, before the link's own are taken out.
inline constexpr uint64_t kEsp32s3ModulePins = kEsp32s3Allowed & ~kEsp32s3Uart0;

} // namespace core::gpio_policy
