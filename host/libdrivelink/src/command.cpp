#include "drivelink/command.hpp"
#include "drivelink/client.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <bit>
#include <cmath>
#include <limits>

namespace drivelink
{
namespace {
bool validBits(ValueType type,uint32_t bits) noexcept {
    switch(type) {
    case ValueType::Bool:return bits<=1;
    case ValueType::U8:case ValueType::Enum:return bits<=255;
    case ValueType::U16:return bits<=65535;
    case ValueType::U32:case ValueType::I32:return true;
    case ValueType::F32:return std::isfinite(std::bit_cast<float>(bits));
    default:return false;
    }
}
}
struct CommandGroup::Impl
{
    uint8_t group,epoch;
    uint16_t counter=0;
    std::vector<ItemDescriptor> descriptors;
    std::array<std::atomic<uint32_t>,64> hold{};
    struct Packet {std::array<uint32_t,64> values{};uint64_t generation=0;};
    std::array<Packet,8> queue;
    std::atomic<uint64_t> head{0},tail{0},generation{0},rejected{0};
    std::atomic<bool> active{true};
    std::atomic_flag producing=ATOMIC_FLAG_INIT;
    uint64_t last=0,interval;
    bool keepalive;
    Impl(uint8_t g,uint8_t e,std::vector<ItemDescriptor> d,uint16_t watchdog,bool k)
        :group(g),epoch(e),descriptors(std::move(d)),interval(uint64_t(watchdog)*1000/3),keepalive(k) {}
};
CommandGroup::CommandGroup(uint8_t group,uint8_t epoch,std::vector<ItemDescriptor> descriptors,
                           std::span<const uint32_t> values,uint16_t watchdog_ms,bool keepalive) {
    if(group>=protocol::defs::kCmdGroupChannelCount || descriptors.size()>64 ||
       descriptors.size()!=values.size() || watchdog_ms<3) {throw Error(ErrorKind::Usage,"invalid managed command group");}
    for(const auto &d:descriptors) {if(d.kind!=ItemKind::Input || d.value_type==ValueType::None) {throw Error(ErrorKind::Usage,"command refs must describe inputs");}}
    m_impl=std::make_unique<Impl>(group,epoch,std::move(descriptors),watchdog_ms,keepalive);hold(values);
}
CommandGroup::~CommandGroup() {stop();}
void CommandGroup::hold(std::span<const uint32_t> values) {
    auto &d=*m_impl;if(values.size()!=d.descriptors.size()) {throw Error(ErrorKind::Usage,"hold width mismatch");}
    for(size_t i=0;i<values.size();++i) {(void)decodeValue(d.descriptors[i].value_type,values[i]);}
    // Hold updates belong to the producer thread, like write().
    for(size_t i=0;i<values.size();++i) {d.hold[i].store(values[i]);}
}
bool CommandGroup::write(std::span<const double> values) noexcept {
    auto &d=*m_impl;
    if(values.size()!=d.descriptors.size()) {++d.rejected;return false;}
    std::array<uint32_t,64> bits{};
    for(size_t i=0;i<values.size();++i) {
        const double value=values[i];
        if(std::isnan(value)) {bits[i]=d.hold[i].load();continue;}
        if(!std::isfinite(value)) {++d.rejected;return false;}
        const auto type=d.descriptors[i].value_type;
        if(type==ValueType::F32) {
            const float f=static_cast<float>(value);if(!std::isfinite(f)) {++d.rejected;return false;}bits[i]=std::bit_cast<uint32_t>(f);
        } else {
            const bool signed_=type==ValueType::I32;
            const double lower=signed_?double(std::numeric_limits<int32_t>::min()):0;
            const double upper=signed_?double(std::numeric_limits<int32_t>::max()):type==ValueType::Bool?1:
                type==ValueType::U8 || type==ValueType::Enum?255:type==ValueType::U16?65535:double(UINT32_MAX);
            if(std::trunc(value)!=value || value<lower || value>upper) {++d.rejected;return false;}
            bits[i]=signed_?std::bit_cast<uint32_t>(static_cast<int32_t>(value)):static_cast<uint32_t>(value);
        }
    }return writeBits({bits.data(),values.size()});
}
bool CommandGroup::writeBits(std::span<const uint32_t> values) noexcept {
    auto &d=*m_impl;
    if(!d.active || values.size()!=d.descriptors.size() || d.producing.test_and_set(std::memory_order_acquire)) {++d.rejected;return false;}
    struct Guard {std::atomic_flag &flag;~Guard(){flag.clear(std::memory_order_release);}} guard{d.producing};
    for(size_t i=0;i<values.size();++i) {if(!validBits(d.descriptors[i].value_type,values[i])) {++d.rejected;return false;}}
    const auto tail=d.tail.load(std::memory_order_relaxed);
    if(tail-d.head.load(std::memory_order_acquire)>=d.queue.size()) {++d.rejected;return false;}
    auto &packet=d.queue[tail%d.queue.size()];packet.generation=d.generation.load();
    std::copy(values.begin(),values.end(),packet.values.begin());d.tail.store(tail+1,std::memory_order_release);return true;
}
bool CommandGroup::poll(uint64_t now,Bytes &payload,uint64_t &generation) {
    auto &d=*m_impl;if(!d.active) {return false;}
    auto head=d.head.load(std::memory_order_relaxed);
    while(head<d.tail.load(std::memory_order_acquire)) {
        const auto packet=d.queue[head%d.queue.size()];d.head.store(++head,std::memory_order_release);
        if(packet.generation!=d.generation.load()) {continue;}
        generation=packet.generation;payload=commandPayload(d.epoch,d.counter++,{packet.values.data(),d.descriptors.size()});d.last=now;return true;
    }
    if(d.keepalive && now-d.last>=d.interval) {generation=d.generation.load();payload=commandPayload(d.epoch,d.counter++,{});d.last=now;return true;}
    return false;
}
void CommandGroup::discardQueued() noexcept {++m_impl->generation;}
void CommandGroup::stop() noexcept {m_impl->active=false;discardQueued();}
bool CommandGroup::active() const noexcept {return m_impl->active;}
uint64_t CommandGroup::rejected() const noexcept {return m_impl->rejected;}
uint8_t CommandGroup::id() const {return m_impl->group;}
uint64_t CommandGroup::queueGeneration() const noexcept {return m_impl->generation;}
}
