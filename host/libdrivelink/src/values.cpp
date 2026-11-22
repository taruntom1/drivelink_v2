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
uint32_t parseValue(ValueType value_type, const std::string &text, std::span<const EnumEntry> enums)
{
    const auto type = static_cast<uint8_t>(value_type);
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
            if (e.name == text)
            {
                return e.value;
            }
        }
        const auto value = parseUnsigned(text, 255);
        for (const auto &e : enums)
        {
            if (e.value == value)
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
Value decodeValue(ValueType type, uint32_t bits) {
    switch(type) {
    case ValueType::Bool: if(bits>1) { throw Error(ErrorKind::Protocol,"invalid boolean slot"); }return bits!=0;
    case ValueType::F32: {
        const auto value=std::bit_cast<float>(bits);
        if(!std::isfinite(value)) { throw Error(ErrorKind::Protocol,"non-finite scalar"); }return value;
    }
    case ValueType::I32: return std::bit_cast<int32_t>(bits);
    case ValueType::U8: if(bits<=255) { return static_cast<uint8_t>(bits); }break;
    case ValueType::U16: if(bits<=65535) { return static_cast<uint16_t>(bits); }break;
    case ValueType::U32: return bits;
    case ValueType::Enum: if(bits<=255) { return EnumValue{static_cast<uint8_t>(bits)}; }break;
    default: break;
    }
    throw Error(ErrorKind::Protocol,"invalid scalar slot");
}
uint32_t valueBits(const Value &value) {
    return std::visit([](const auto &v)->uint32_t {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,EnumValue>) { return v.value; }
        else if constexpr(std::is_same_v<T,float> || std::is_same_v<T,int32_t>) {
            if constexpr(std::is_same_v<T,float>) { if(!std::isfinite(v)) { throw Error(ErrorKind::Usage,"non-finite scalar"); } }
            return std::bit_cast<uint32_t>(v);
        } else { return static_cast<uint32_t>(v); }
    },value);
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
