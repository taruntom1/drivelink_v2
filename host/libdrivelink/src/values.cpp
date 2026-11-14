#include "drivelink/client.hpp"
#include <bit>
#include <charconv>
#include <cmath>

namespace drivelink
{
namespace
{
[[noreturn]] void invalid(const std::string &text)
{
    throw Error(ErrorKind::Usage, "invalid value: " + text);
}
} // namespace
uint32_t parseUnsigned(const std::string &text, uint32_t max)
{
    uint32_t value = 0;
    int base = 10;
    size_t offset = 0;
    if (text.starts_with("0x"))
    {
        base = 16;
        offset = 2;
    }
    const auto result = std::from_chars(text.data() + offset, text.data() + text.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > max)
    {
        invalid(text);
    }
    return value;
}
Ref parseRef(const std::string &text)
{
    const auto a = text.find(':'), b = text.find(':', a == std::string::npos ? text.size() : a + 1);
    if (a == std::string::npos || b == std::string::npos)
    {
        invalid(text);
    }
    return {static_cast<uint8_t>(parseUnsigned(text.substr(0, a), 255)),
            static_cast<uint8_t>(parseUnsigned(text.substr(a + 1, b - a - 1), 254)),
            static_cast<uint16_t>(parseUnsigned(text.substr(b + 1), 65535))};
}
uint32_t parseValue(const Json &d, const std::string &text, const Json &enums)
{
    const auto type = d.at("value_type").get<uint8_t>();
    if (type == 0)
    {
        if (text == "true" || text == "1")
        {
            return 1;
        }
        if (text == "false" || text == "0")
        {
            return 0;
        }
        invalid(text);
    }
    if (type == 5)
    {
        float value = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !std::isfinite(value))
        {
            invalid(text);
        }
        return std::bit_cast<uint32_t>(value);
    }
    if (type == 4)
    {
        int32_t value = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
        {
            invalid(text);
        }
        return std::bit_cast<uint32_t>(value);
    }
    if (type == 6)
    {
        for (const auto &e : enums)
        {
            if (e.at("name") == text)
            {
                return e.at("value").get<uint32_t>();
            }
        }
        const auto value = parseUnsigned(text, 255);
        for (const auto &e : enums)
        {
            if (e.at("value") == value)
            {
                return value;
            }
        }
        invalid(text);
    }
    if (type > 3)
    {
        invalid(text);
    }
    return parseUnsigned(text, type == 1 ? 255 : type == 2 ? 65535 : UINT32_MAX);
}
Json valueJson(uint8_t type, uint32_t bits)
{
    if (type == 0)
    {
        if (bits > 1)
        {
            throw Error(ErrorKind::Protocol, "invalid boolean slot");
        }
        return bits != 0;
    }
    if (type == 5)
    {
        const auto f = std::bit_cast<float>(bits);
        if (!std::isfinite(f))
        {
            throw Error(ErrorKind::Protocol, "non-finite scalar");
        }
        return f;
    }
    if (type == 4)
    {
        return std::bit_cast<int32_t>(bits);
    }
    if (type > 6 || (type == 1 && bits > 255) || (type == 2 && bits > 65535) || (type == 6 && bits > 255))
    {
        throw Error(ErrorKind::Protocol, "invalid scalar slot");
    }
    return bits;
}
Bytes parseHex(const std::string &text)
{
    if (text.size() % 2 || text.size() > 1024)
    {
        invalid(text);
    }
    Bytes out;
    for (size_t i = 0; i < text.size(); i += 2)
    {
        unsigned value = 0;
        auto r = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
        if (r.ec != std::errc{} || r.ptr != text.data() + i + 2)
        {
            invalid(text);
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return out;
}
std::string hex(std::span<const uint8_t> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes)
    {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}
} // namespace drivelink
