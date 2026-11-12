#pragma once
#include "drivelink/session.hpp"
#include <array>
#include <nlohmann/json.hpp>
static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR == 11 &&
                  NLOHMANN_JSON_VERSION_PATCH == 3,
              "libdrivelink's public JSON ABI requires nlohmann JSON 3.11.3");

namespace drivelink
{
using Json = nlohmann::json;
struct Ref
{
    uint8_t type = 0, instance = 0;
    uint16_t item = 0;
};
// First service slice, without a cached device model. All descriptors come
// from the connected device. The caller owns the Session and serializes Client.
class Client
{
  public:
    explicit Client(Session &session) : m_session(session) {}
    Json info(); // validates the core protocol version, learns payload cap
    Json types();
    Json items(uint8_t type);
    Json enums(uint8_t type, uint16_t item);
    Json codes();
    Json instances(bool pending = false);
    Json add(uint8_t type, const std::string &label, uint8_t loop = 255);
    Json remove(uint8_t type, uint8_t instance);
    Json label(uint8_t type, uint8_t instance, const std::string &label);
    Json discard();
    Json system(uint8_t op);
    Json get(std::span<const Ref> refs, bool pending = false);
    Json set(std::span<const Ref> refs, std::span<const uint32_t> values);
    Json action(Ref ref, std::array<uint32_t, 4> args = {}, std::span<const uint8_t> blob = {});
    Json diagInfo();
    Json diagEntries();
    Json diagClear(uint8_t what = 3);
    Json event(const Frame &frame) const;
    Json item(Ref ref);

  private:
    Reply accepted(uint8_t family, uint8_t op, std::span<const uint8_t> body = {});
    Json pages(uint8_t family, uint8_t op, const Bytes &prefix = {});
    Session &m_session;
};
// Descriptor types use the existing ValueType wire encoding. These functions
// reject truncation, trailing junk, overflow, invalid bool/enum, NaN and Inf.
uint32_t parseValue(const Json &descriptor, const std::string &text, const Json &enums = Json::array());
Json valueJson(uint8_t type, uint32_t bits);
Bytes parseHex(const std::string &text);
std::string hex(std::span<const uint8_t> bytes);
Ref parseRef(const std::string &text);
uint32_t parseUnsigned(const std::string &text, uint32_t max = UINT32_MAX);
} // namespace drivelink
