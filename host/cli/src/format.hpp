#pragma once
#include "drivelink/agent.hpp"
#include "drivelink/client.hpp"
#include <nlohmann/json.hpp>
namespace cli
{
using Json = nlohmann::json;
using namespace drivelink;
Json toJson(const Value &v);
Json toJson(const Ref &v);
Json toJson(const EnumEntry &v);
Json toJson(const ScalarDescriptor &v);
Json toJson(const TypeDescriptor &v);
Json toJson(const ItemDescriptor &v);
Json toJson(const CodeDescriptor &v);
Json toJson(const Instance &v);
Json toJson(const LoopInfo &v);
Json toJson(const DeviceInfo &v);
Json toJson(const OperationResult &v);
Json toJson(const SystemResult &v);
Json toJson(const LayoutResult &v);
Json toJson(const AddedInstance &v);
Json toJson(const ParamResult &v);
Json toJson(const ActionResult &v);
Json toJson(const DiagInfo &v);
Json toJson(const DiagEntry &v);
Json toJson(const Group &v);
Json toJson(const GroupList &v);
Json toJson(const GroupMutation &v);
Json toJson(const CaptureTrigger &v);
Json toJson(const CaptureStatus &v);
Json toJson(const CapturePage &v);
Json toJson(const CaptureData &v);
Json toJson(const TestgenConfig &v);
Json toJson(const TestgenStarted &v);
Json toJson(const TestgenStatus &v);
Json toJson(const OtaStatus &v);
Json toJson(const LoopOverruns &v);
Json toJson(const StatusFrame &v);
Json toJson(const CaptureDone &v);
Json toJson(const TestgenDone &v);
Json toJson(const DiagRecord &v);
Json toJson(const CounterDelta &v);
Json toJson(const DiagCounters &v);
Json toJson(const DiagCleared &v);
Json toJson(const BootEvent &v);
Json toJson(const SubscriptionSample &v);
Json toJson(const Frame &v);
Json toJson(const Event &v);
template <class T> Json toJson(const std::vector<T> &values)
{
    Json out = Json::array();
    for (const auto &v : values)
    {
        out.push_back(toJson(v));
    }
    return out;
}
Json toJson(const agent::DeviceRecord &v);
Json toJson(const agent::ClientRecord &v);
// "ALIAS online|offline ID NAME ENDPOINT clients=N [holder=ID:NAME]" (ID and NAME are "-" when empty)
std::string deviceLine(const agent::DeviceRecord &v);
// "client_id NAME [holder]"
std::string clientLine(const agent::ClientRecord &v);
Json valueJson(ValueType type, uint32_t bits);
template<class T> Json toJson(const std::optional<T> &value) {
    return value ? toJson(*value) : Json(nullptr);
}
} // namespace cli
