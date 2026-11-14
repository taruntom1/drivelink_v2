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
// Capture trigger: kind manual 0, testgen_start 1, value_change 2, threshold 3;
// edge rising 0, falling 1; value is the threshold slot bits.
struct CaptureTrigger
{
    uint8_t kind = 0;
    Ref ref{};
    uint8_t edge = 0;
    uint32_t value = 0;
};
struct TestgenConfig
{
    uint8_t loop = 0;
    Ref target{};
    uint8_t waveform = 0; // step, square, ramp, sine, chirp_linear, chirp_log
    float offset = 0, amplitude = 0, frequency_start = 0, frequency_end = 0;
    uint32_t duration_ms = 1000, repeat = 1; // zero repeat: until STOP/DISARM
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
    Json arm() { return system(2); }
    Json disarm() { return system(3); }
    Json acknowledgeFault() { return system(4); }
    Json benchStall(uint8_t loop, uint16_t duration_ms);
    Json get(std::span<const Ref> refs, bool pending = false);
    Json set(std::span<const Ref> refs, std::span<const uint32_t> values);
    Json action(Ref ref, std::array<uint32_t, 4> args = {}, std::span<const uint8_t> blob = {});
    Json diagInfo();
    Json diagEntries();
    Json diagClear(uint8_t what = 3);
    Json event(const Frame &frame) const;
    Json item(Ref ref);
    Json groups();
    Json subConfigure(uint8_t group, uint8_t loop, uint16_t divider, std::span<const Ref> refs);
    Json cmdConfigure(uint8_t group, uint8_t loop, std::span<const Ref> refs);
    Json subEnable(uint8_t group, bool enabled);
    Json groupClear(uint8_t group, bool command);
    bool command(uint8_t group, uint8_t epoch, uint16_t counter, std::span<const uint32_t> values = {});
    Json captureConfig(uint8_t loop, uint16_t divider, uint32_t samples, uint32_t pre_trigger,
                       const CaptureTrigger &trigger, std::span<const Ref> refs);
    Json captureArm();
    Json captureTrigger();
    Json captureAbort();
    Json captureStatus();
    // One READ page: `width` is the configured ref count.
    Json captureRead(uint32_t first, uint16_t count, size_t width);
    // STATUS of a Done capture plus every sample (`data`, rows of slot bits),
    // paged within the device payload (call info() first to learn it).
    Json captureData();
    Json testgenStart(const TestgenConfig &config);
    Json testgenStop(uint8_t loop);
    Json testgenStatus(uint8_t loop);

  private:
    Reply accepted(uint8_t family, uint8_t op, std::span<const uint8_t> body = {});
    Json pages(uint8_t family, uint8_t op, const Bytes &prefix = {});
    Json groupMutation(uint8_t op, const Bytes &body);
    Json captureOp(uint8_t op);
    Session &m_session;
    uint16_t m_max_payload = 1024;
};
// Descriptor types use the existing ValueType wire encoding. These functions
// reject truncation, trailing junk, overflow, invalid bool/enum, NaN and Inf.
uint32_t parseValue(const Json &descriptor, const std::string &text, const Json &enums = Json::array());
Json valueJson(uint8_t type, uint32_t bits);
Bytes parseHex(const std::string &text);
std::string hex(std::span<const uint8_t> bytes);
Ref parseRef(const std::string &text);
uint8_t parseCaptureTrigger(const std::string &text); // manual, value_change, threshold, ...
uint8_t parseTestgenWaveform(const std::string &text);
uint32_t parseUnsigned(const std::string &text, uint32_t max = UINT32_MAX);
Bytes commandPayload(uint8_t epoch, uint16_t counter, std::span<const uint32_t> values);
// Returns null for a stale epoch/channel. Descriptor order matches group refs.
Json decodeSubscription(const Frame &frame, const Json &group, const Json &descriptors);
} // namespace drivelink
