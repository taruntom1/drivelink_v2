#pragma once
#include "drivelink/session.hpp"
#include "drivelink/types.hpp"
#include <functional>

namespace drivelink {
// Synchronous typed services, without a cached device model. Descriptors come
// from the connected device. The caller owns the Session and serializes Client.
class Client
{
  public:
    explicit Client(Session &session) : m_session(session), m_max_payload(session.maxPayload()) {}
    // Own all arguments in operation. A fresh Client on the session's serial
    // service executor shares the same typed services and parsers as sync calls.
    template<class F>
    RequestHandle async(F operation, Completion<std::invoke_result_t<F, Client &>> completion)
    {
        using T=std::invoke_result_t<F,Client &>;
        return m_session.async<T>([session=&m_session, operation=std::move(operation)]() mutable -> T {
            Client client(*session);return operation(client);
        }, std::move(completion));
    }
    Session &session() const { return m_session; }
    DeviceInfo info(); // validates the core protocol version, learns payload cap
    std::vector<TypeDescriptor> types();
    std::vector<ItemDescriptor> items(uint8_t type);
    std::vector<EnumEntry> enums(uint8_t type, uint16_t item);
    std::vector<CodeDescriptor> codes();
    std::vector<Instance> instances(bool pending = false);
    AddedInstance add(uint8_t type, const std::string &label, uint8_t loop = 255);
    LayoutResult remove(uint8_t type, uint8_t instance);
    LayoutResult label(uint8_t type, uint8_t instance, const std::string &label);
    OperationResult discard();
    SystemResult system(uint8_t op);
    SystemResult arm() { return system(2); }
    SystemResult disarm() { return system(3); }
    // Bypasses the serial service executor and reserves emergency admission.
    RequestHandle disarmAsync(Completion<SystemResult> completion);
    SystemResult acknowledgeFault() { return system(4); }
    OperationResult benchStall(uint8_t loop, uint16_t duration_ms);
    ParamResult get(std::span<const Ref> refs, bool pending = false);
    ParamResult set(std::span<const Ref> refs, std::span<const uint32_t> values);
    ActionResult action(Ref ref, std::array<uint32_t, 4> args = {}, std::span<const uint8_t> blob = {});
    DiagInfo diagInfo();
    std::vector<DiagEntry> diagEntries();
    OperationResult diagClear(uint8_t what = 3);
    Event event(const Frame &frame) const;
    static Event decodeEvent(const Frame &frame);
    ItemDescriptor item(Ref ref);
    GroupList groups();
    GroupMutation subConfigure(uint8_t group, uint8_t loop, uint16_t divider, std::span<const Ref> refs);
    GroupMutation cmdConfigure(uint8_t group, uint8_t loop, std::span<const Ref> refs);
    GroupMutation subEnable(uint8_t group, bool enabled);
    GroupMutation groupClear(uint8_t group, bool command);
    bool command(uint8_t group, uint8_t epoch, uint16_t counter, std::span<const uint32_t> values = {});
    std::shared_ptr<CommandGroup> managedCommand(uint8_t group,uint8_t loop,std::span<const Ref> refs,
                                                std::span<const uint32_t> hold);
    OperationResult captureConfig(uint8_t loop, uint16_t divider, uint32_t samples, uint32_t pre_trigger,
                       const CaptureTrigger &trigger, std::span<const Ref> refs);
    OperationResult captureArm();
    OperationResult captureTrigger();
    OperationResult captureAbort();
    CaptureStatus captureStatus();
    // One READ page: `width` is the configured ref count.
    CapturePage captureRead(uint32_t first, uint16_t count, size_t width);
    // STATUS of a Done capture plus every sample (rows of slot bits),
    // paged within the device payload (call info() first to learn it).
    CaptureData captureData();
    TestgenStarted testgenStart(const TestgenConfig &config);
    OperationResult testgenStop(uint8_t loop);
    TestgenStatus testgenStatus(uint8_t loop);
    OtaStatus otaStatus();
    uint32_t otaBegin(uint32_t size,const protocol::Sha256Digest &digest);
    uint32_t otaWrite(uint32_t offset,std::span<const uint8_t> bytes);
    OperationResult otaFinish();
    OperationResult otaAbort();
    OperationResult otaConfirm();
    // Upload/FINISH only. Caller owns reboot, reconnect and confirmation.
    // Progress returns false to cancel; cancellation/error attempts ABORT.
    OtaStatus otaUpload(std::span<const uint8_t> image,uint8_t window=4,
                   const std::function<bool(uint32_t,uint32_t)> &progress={});

  private:
    static void contextualize(Error &error, uint8_t family, uint8_t op, bool mutation);
    Reply accepted(uint8_t family, uint8_t op, std::span<const uint8_t> body = {});
    template<class T> std::vector<T> pages(uint8_t family, uint8_t op, const Bytes &prefix = {});
    GroupMutation groupMutation(uint8_t op, const Bytes &body);
    OperationResult captureOp(uint8_t op);
    Session &m_session;
    uint16_t m_max_payload = 1024;
};
// Descriptor types use the existing ValueType wire encoding. These functions
// reject truncation, trailing junk, overflow, invalid bool/enum, NaN and Inf.
Bytes parseHex(const std::string &text);
Bytes readOtaImage(const std::string &path);
std::string hex(std::span<const uint8_t> bytes);
Ref parseRef(const std::string &text);
uint8_t parseCaptureTrigger(const std::string &text); // manual, value_change, threshold, ...
uint8_t parseTestgenWaveform(const std::string &text);
uint32_t parseUnsigned(const std::string &text, uint32_t max = UINT32_MAX);
Bytes commandPayload(uint8_t epoch, uint16_t counter, std::span<const uint32_t> values);
// Returns nullopt for a stale epoch/channel. Descriptor order matches group refs.
std::optional<SubscriptionSample> decodeSubscription(const Frame &frame, const Group &group, std::span<const ItemDescriptor> descriptors);
} // namespace drivelink
