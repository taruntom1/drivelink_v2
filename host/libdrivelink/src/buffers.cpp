#include "drivelink/buffers.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include <bit>
#include <algorithm>

namespace drivelink
{
namespace
{
constexpr size_t kWords=71;
using Words=std::array<uint64_t,kWords>;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
Words encode(const GroupSample &s,uint64_t ticket) noexcept {
    Words w{};
    w[0]=ticket;w[1]=uint64_t(s.group)|(uint64_t(s.epoch)<<8)|(uint64_t(s.count)<<16)|(uint64_t(s.counter)<<32);
    w[2]=s.device_us;w[3]=s.generation;w[4]=std::bit_cast<uint64_t>(s.host_us);
    w[5]=std::bit_cast<uint64_t>(s.uncertainty_us);w[6]=s.mapped;
    for(size_t i=0;i<64;++i) {w[7+i]=s.values[i];}return w;
}
GroupSample decode(const Words &w) noexcept {
    GroupSample s;s.group=static_cast<uint8_t>(w[1]);s.epoch=static_cast<uint8_t>(w[1]>>8);
    s.count=static_cast<uint8_t>(w[1]>>16);s.counter=static_cast<uint16_t>(w[1]>>32);
    s.device_us=w[2];s.generation=w[3];s.host_us=std::bit_cast<double>(w[4]);
    s.uncertainty_us=std::bit_cast<double>(w[5]);s.mapped=w[6]!=0;
    for(size_t i=0;i<64;++i) {s.values[i]=static_cast<uint32_t>(w[7+i]);}return s;
}
struct Slot
{
    std::atomic<uint64_t> sequence{0};
    std::array<std::atomic<uint64_t>,kWords> words{};
    void write(const Words &w) noexcept {
        sequence.fetch_add(1,std::memory_order_seq_cst);
        // Atomic payload words are required: a seqlock around ordinary C++
        // fields is a data race, even when the reader rejects its copy.
        for(size_t i=0;i<kWords;++i) {words[i].store(w[i],std::memory_order_seq_cst);}
        sequence.fetch_add(1,std::memory_order_seq_cst);
    }
    bool read(Words &w) const noexcept {
        for(unsigned attempt=0;attempt<8;++attempt) {
            const auto before=sequence.load(std::memory_order_seq_cst);if(before&1) {continue;}
            for(size_t i=0;i<kWords;++i) {w[i]=words[i].load(std::memory_order_seq_cst);}
            if(before==sequence.load(std::memory_order_seq_cst)) {return w[0]!=0;}
        }return false;
    }
};
}
struct GroupBuffer::Impl
{
    uint8_t group,epoch;
    size_t width,capacity;
    std::unique_ptr<Slot[]> ring;
    Slot latest;
    std::atomic<uint64_t> head{0},tail{0},lost{0},bad{0};
    std::atomic<bool> valid{true};
    Impl(uint8_t g,uint8_t e,size_t w,size_t c):group(g),epoch(e),width(w),capacity(c),ring(std::make_unique<Slot[]>(c)) {}
};
GroupBuffer::GroupBuffer(uint8_t group,uint8_t epoch,size_t width,size_t capacity) {
    if(group>=protocol::defs::kSubGroupChannelCount || width>64 || !capacity || capacity>65536) {
        throw Error(ErrorKind::Usage,"invalid group buffer limits");
    }m_impl=std::make_unique<Impl>(group,epoch,width,capacity);
}
GroupBuffer::~GroupBuffer()=default;
void GroupBuffer::receive(const Frame &frame) noexcept {
    auto &d=*m_impl;if(!d.valid || frame.type!=protocol::defs::kSubGroupChannelFirst+d.group) {return;}
    if(frame.payload.size()<2 || frame.payload[1]!=d.epoch) {return;}
    if(frame.payload.size()!=12+4*d.width || frame.payload[0]!=protocol::core_wire::kVersion) {++d.bad;return;}
    protocol::wire::Reader r(frame.payload.data(),frame.payload.size());(void)r.readU8();(void)r.readU8();
    GroupSample s;s.group=d.group;s.epoch=d.epoch;s.count=static_cast<uint8_t>(d.width);
    s.counter=r.readU16Le();s.device_us=r.readU64Le();s.generation=frame.generation;
    s.mapped=frame.clock.offset_valid;s.host_us=frame.clock.map(s.device_us);
    s.uncertainty_us=frame.clock.uncertainty(s.device_us);
    for(size_t i=0;i<d.width;++i) {s.values[i]=r.readU32Le();}
    const auto pos=d.head.load(std::memory_order_relaxed);auto tail=d.tail.load(std::memory_order_acquire);
    while(pos-tail>=d.capacity) {
        const auto next=pos-d.capacity+1;
        if(d.tail.compare_exchange_weak(tail,next,std::memory_order_acq_rel)) {d.lost.fetch_add(next-tail);break;}
    }
    const auto words=encode(s,pos+1);d.ring[pos%d.capacity].write(words);d.latest.write(words);
    d.head.store(pos+1,std::memory_order_release);
}
bool GroupBuffer::latest(GroupSample &sample) const noexcept {
    const auto &d=*m_impl;Words w{};
    if(!d.valid || !d.latest.read(w) || !d.valid) {return false;}sample=decode(w);return true;
}
bool GroupBuffer::pop(GroupSample &sample) noexcept {
    auto &d=*m_impl;
    for(unsigned attempt=0;attempt<8;++attempt) {
        if(!d.valid) {return false;}auto tail=d.tail.load(std::memory_order_acquire);
        if(tail>=d.head.load(std::memory_order_acquire)) {return false;}
        Words w{};if(!d.ring[tail%d.capacity].read(w) || w[0]!=tail+1) {continue;}
        if(d.tail.compare_exchange_strong(tail,tail+1,std::memory_order_acq_rel)) {
            if(!d.valid) {return false;}sample=decode(w);return true;
        }
    }return false;
}
uint64_t GroupBuffer::dropped() const noexcept {return m_impl->lost.load();}
uint64_t GroupBuffer::malformed() const noexcept {return m_impl->bad.load();}
void GroupBuffer::invalidate() noexcept {m_impl->valid=false;}
}
