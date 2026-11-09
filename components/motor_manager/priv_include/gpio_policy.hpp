#pragma once

/// Which GPIOs a host may assign to a motor (CreateMotor). Pure constexpr
/// code with no ESP-IDF dependency, so the host unit tests cover it.
/// motor_manager.cpp checks kGpioCount against the IDF's GPIO_NUM_MAX.

#include "motor_manager_protocol.hpp"

#include <cstdint>

namespace motor_manager::gpio_policy {

constexpr uint64_t gpioBit(unsigned gpio)
{
    return (gpio < 64U) ? (uint64_t{1} << gpio) : 0U;
}

constexpr uint64_t gpioRange(unsigned first, unsigned last)
{
    uint64_t mask = 0;
    for (unsigned g = first; g <= last; ++g) {
        mask |= gpioBit(g);
    }
    return mask;
}

/// ESP32-S3 GPIO numbers run 0..48 (GPIO_NUM_MAX = 49).
constexpr unsigned kGpioCount = 49;

/// Numbers in 0..48 that have no GPIO on the ESP32-S3.
constexpr uint64_t kNonexistentMask = gpioRange(22, 25);

/// Pins a host may never claim, whatever the board:
/// - 19, 20: USB D-/D+ (the console runs on USB-Serial-JTAG)
/// - 26..32: SPI flash / quad PSRAM
/// - 33..37: octal flash / PSRAM (reserved on every module, so a board
///   change cannot make them unsafe)
constexpr uint64_t kReservedMask = gpioBit(19) | gpioBit(20) | gpioRange(26, 32) | gpioRange(33, 37);

/// True if a host may use `gpio`: it exists, it is not reserved, and it is
/// not in `extra_reserved` (for example the uart_comm TX/RX pins).
constexpr bool isAllowed(uint8_t gpio, uint64_t extra_reserved)
{
    if (gpio >= kGpioCount) {
        return false;
    }
    return ((kNonexistentMask | kReservedMask | extra_reserved) & gpioBit(gpio)) == 0U;
}

/// The four pins of a motor as a bit mask (one bit per pin).
constexpr uint64_t motorPinMask(const MotorConfigPayload &cfg)
{
    return gpioBit(cfg.pwm_gpio) | gpioBit(cfg.dir_gpio) | gpioBit(cfg.enc_a) | gpioBit(cfg.enc_b);
}

/// True if all four motor pins are allowed and no two are the same.
constexpr bool areMotorPinsAllowed(const MotorConfigPayload &cfg, uint64_t extra_reserved)
{
    const uint8_t pins[] = {cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b};
    for (const uint8_t pin : pins) {
        if (!isAllowed(pin, extra_reserved)) {
            return false;
        }
    }
    // Four allowed pins (all < 49) give four distinct bits only if distinct.
    const uint64_t mask = motorPinMask(cfg);
    unsigned bits = 0;
    for (unsigned g = 0; g < kGpioCount; ++g) {
        bits += ((mask & gpioBit(g)) != 0U) ? 1U : 0U;
    }
    return bits == 4U;
}

} // namespace motor_manager::gpio_policy
