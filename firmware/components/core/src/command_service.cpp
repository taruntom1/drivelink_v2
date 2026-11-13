#include "core/command_service.hpp"
#include "drivelink/protocol/gen/types.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include <algorithm>
#include <cstring>

namespace core {
size_t fastDisarm(Core &core, std::span<const uint8_t> request, std::span<uint8_t> response) {
    namespace cw = drivelink::protocol::core_wire;
    cw::Header h;
    drivelink::protocol::wire::Reader r(request.data(), request.size());
    const bool valid = cw::decodeHeader(r, h) && r.done() &&
        h.family == static_cast<uint8_t>(defs::Family::System) && h.op == static_cast<uint8_t>(defs::SystemOp::Disarm);
    if (response.size() < cw::kResponseHeader) { return 0; }
    if (valid) { core.disarm(); }
    drivelink::protocol::wire::Writer w(response.data(), response.size());
    cw::response(w, h, valid ? Result::Ok : Result::InvalidArgument, valid ? Detail::None : Detail::BadValue);
    return w.size();
}
namespace cw = drivelink::protocol::core_wire;
namespace wire = drivelink::protocol::wire;
namespace {
Status bad(Detail d=Detail::BadValue) { return Status::fail(Result::InvalidArgument,d); }
Ref ref(wire::Reader &r) { Ref v{}; v.type=r.readU8(); v.instance=r.readU8(); v.id=r.readU16Le(); return v; }
bool label(wire::Reader &r, char (&s)[kLabelBytes+1]) {
    const auto n=r.readU8();
    if (!n || n>kLabelBytes || r.remaining()!=n) { return false; }
    for (size_t i=0;i<n;++i) { s[i]=static_cast<char>(r.readU8()); if (!s[i]) { return false; } }
    s[n]=0; return r.done();
}
void page(wire::Writer &w,uint16_t total,uint16_t next,uint8_t n) { w.appendU16Le(total);w.appendU16Le(next);w.appendU8(n); }
}
static uint8_t captureFlags(const CaptureInfo &info, const CaptureSnapshot &snap)
{
    return static_cast<uint8_t>((info.configured ? 1 : 0) | (snap.irregular ? 2 : 0));
}
size_t captureDoneEvent(Core &core, std::span<uint8_t> payload)
{
    CaptureSnapshot snap;
    if (payload.size() < kCaptureDoneEventBytes || !core.capture().takeDone(snap)) { return 0; }
    wire::Writer w(payload.data(), payload.size());
    w.appendU8(cw::kVersion); w.appendU8(static_cast<uint8_t>(defs::CoreEvent::CaptureDone));
    w.appendU32Le(snap.recorded); w.appendU32Le(snap.trigger_index);
    w.appendU8(captureFlags(core.capture().info(), snap));
    return w.size();
}
size_t testgenDoneEvent(Core &core, std::span<uint8_t> payload)
{
    uint8_t loop = 0; TestgenSnapshot snap;
    if (payload.size() < 8 || !core.testgen().takeDone(loop, snap)) { return 0; }
    wire::Writer w(payload.data(), payload.size());
    w.appendU8(cw::kVersion); w.appendU8(static_cast<uint8_t>(defs::CoreEvent::TestgenDone));
    w.appendU8(loop); w.appendU32Le(snap.run); w.appendU8(static_cast<uint8_t>(snap.reason));
    return w.size();
}
Status CommandService::testgen(cw::Header h, wire::Reader &r, wire::Writer &w)
{
    auto &gen = m_core.testgen();
    const auto loop = r.readU8();
    if (!r.ok()) { return bad(); }
    switch (static_cast<defs::TestgenOp>(h.op)) {
    case defs::TestgenOp::Start: {
        TestgenConfig c; c.loop = loop; c.target = ref(r);
        c.waveform = static_cast<TestgenWaveform>(r.readU8());
        c.offset = defs::f32FromBits(r.readU32Le()); c.amplitude = defs::f32FromBits(r.readU32Le());
        c.frequency_start = defs::f32FromBits(r.readU32Le()); c.frequency_end = defs::f32FromBits(r.readU32Le());
        c.duration_ms = r.readU32Le(); c.repeat = r.readU32Le();
        if (!r.done()) { return bad(); }
        if (w.remaining() < 4) { return bad(Detail::BufferTooSmall); }
        uint32_t run = 0; const auto st = gen.start(c, run);
        if (st.ok()) { w.appendU32Le(run); }
        return st;
    }
    case defs::TestgenOp::Stop:
        if (!r.done()) { return bad(); }
        return gen.stop(loop);
    case defs::TestgenOp::Status: {
        if (!r.done()) { return bad(); }
        if (w.remaining() < 48) { return bad(Detail::BufferTooSmall); }
        TestgenSnapshot snap; const auto st = gen.snapshot(loop, snap);
        if (!st.ok()) { return st; }
        const auto &c = *gen.config(loop);
        w.appendU8(loop); w.appendU8(static_cast<uint8_t>(snap.state)); w.appendU8(static_cast<uint8_t>(snap.reason));
        w.appendU32Le(snap.run); w.appendU32Le(snap.elapsed_ms); w.appendU32Le(snap.repetitions); w.appendU32Le(snap.value_bits);
        w.appendU8(c.target.type); w.appendU8(c.target.instance); w.appendU16Le(c.target.id);
        w.appendU8(static_cast<uint8_t>(c.waveform));
        w.appendU32Le(defs::f32Bits(c.offset)); w.appendU32Le(defs::f32Bits(c.amplitude));
        w.appendU32Le(defs::f32Bits(c.frequency_start)); w.appendU32Le(defs::f32Bits(c.frequency_end));
        w.appendU32Le(c.duration_ms); w.appendU32Le(c.repeat);
        return {};
    }
    default: return bad(Detail::Unsupported);
    }
}
Status CommandService::capture(cw::Header h,wire::Reader &r,wire::Writer &w)
{
    auto &cap=m_core.capture();
    switch (static_cast<defs::CaptureOp>(h.op)) {
    case defs::CaptureOp::Config: {
        const auto loop=r.readU8();const auto divider=r.readU16Le();
        const auto samples=r.readU32Le();const auto pre=r.readU32Le();
        CaptureTrigger trigger;
        const auto kind=r.readU8();trigger.ref=ref(r);const auto edge=r.readU8();trigger.value=r.readU32Le();
        const auto count=r.readU8();
        if(!r.ok() || r.remaining()!=4U*count) { return bad(); }
        if(count>kMaxCaptureRefs) { return bad(Detail::BatchTooLarge); }
        if(kind>static_cast<uint8_t>(CaptureTriggerKind::Threshold) || edge>static_cast<uint8_t>(CaptureEdge::Falling)) { return bad(); }
        trigger.kind=static_cast<CaptureTriggerKind>(kind);trigger.edge=static_cast<CaptureEdge>(edge);
        for(size_t i=0;i<count;++i) { m_refs[i]=ref(r); }
        return cap.configure(loop,divider,samples,pre,trigger,{m_refs.data(),count});
    }
    case defs::CaptureOp::Arm: if(!r.done()) { return bad(); } return cap.arm();
    case defs::CaptureOp::Abort: if(!r.done()) { return bad(); } return cap.abort();
    case defs::CaptureOp::Trigger: if(!r.done()) { return bad(); } return cap.trigger();
    case defs::CaptureOp::Status: {
        if(!r.done()) { return bad(); }
        const auto &info=cap.info();const auto snap=cap.snapshot();
        if(w.remaining()<44U+4U*info.count) { return bad(Detail::BufferTooSmall); }
        w.appendU8(static_cast<uint8_t>(snap.state));w.appendU8(captureFlags(info,snap));
        w.appendU8(info.loop);w.appendU16Le(info.divider);w.appendU32Le(info.samples);w.appendU32Le(info.pre_trigger);
        w.appendU8(static_cast<uint8_t>(info.trigger.kind));
        w.appendU8(info.trigger.ref.type);w.appendU8(info.trigger.ref.instance);w.appendU16Le(info.trigger.ref.id);
        w.appendU8(static_cast<uint8_t>(info.trigger.edge));w.appendU32Le(info.trigger.value);
        w.appendU32Le(snap.recorded);w.appendU32Le(snap.trigger_index);w.appendU32Le(snap.period_us);
        w.appendU64Le(static_cast<uint64_t>(snap.trigger_us));w.appendU8(info.count);
        for(size_t i=0;i<info.count;++i) {
            w.appendU8(info.refs[i].type);w.appendU8(info.refs[i].instance);w.appendU16Le(info.refs[i].id);
        }
        return {};
    }
    case defs::CaptureOp::Read: {
        const auto first=r.readU32Le();const auto count=r.readU16Le();
        if(!r.done() || !count) { return bad(); }
        if(cap.snapshot().state!=CaptureState::Done) { return bad(Detail::WrongState); }
        const size_t n=cap.info().count;
        const size_t room=std::min<size_t>(kCaptureReadBytes,w.remaining()>6?w.remaining()-6:0);
        if(!n || static_cast<size_t>(count)*n*4U>room) { return bad(Detail::BufferTooSmall); }
        const auto values=std::span(m_capture_values.data(),static_cast<size_t>(count)*n);
        const auto st=cap.read(first,count,values);
        if(!st.ok()) { return st; }
        w.appendU32Le(first);w.appendU16Le(count);
        for(auto v:values) { w.appendU32Le(v); }
        return {};
    }
    default: return bad(Detail::Unsupported);
    }
}
size_t CommandService::handle(std::span<const uint8_t> request,std::span<uint8_t> response)
{
    m_after=AfterReply::None;
    const size_t cap=std::min<size_t>(response.size(),m_info.max_payload);
    if (cap<cw::kResponseHeader) { return 0; }
    wire::Reader r(request.data(),request.size()); cw::Header h;
    const bool parsed=cw::decodeHeader(r,h);
    wire::Writer body(response.data()+cw::kResponseHeader,cap-cw::kResponseHeader);
    Status st=parsed ? dispatch(h,r,body) : bad();
    if (!body.ok()) { st=bad(Detail::BufferTooSmall); m_after=AfterReply::None; }
    wire::Writer header(response.data(),cap);
    cw::response(header,h,st.result,st.detail);
    return cw::kResponseHeader+(st.accepted() ? body.size() : 0);
}
Status CommandService::dispatch(cw::Header h,wire::Reader &r,wire::Writer &w)
{
    switch (static_cast<defs::Family>(h.family)) {
    case defs::Family::System: {
        if (static_cast<defs::SystemOp>(h.op) == defs::SystemOp::BenchStall) {
            const auto loop = r.readU8(); const auto ms = r.readU16Le();
            if (!r.done() || !ms || ms > 10000) { return bad(); }
            if (!m_info.bench_crash) { return bad(Detail::Unsupported); }
            auto *target = m_core.findLoop(loop);
            if (!target) { return bad(Detail::LoopMismatch); }
            target->benchStall(static_cast<uint32_t>(ms) * 1000U); return {};
        }
        if (!r.done()) { return bad(); }
        switch (static_cast<defs::SystemOp>(h.op)) {
        case defs::SystemOp::GetInfo:
            w.appendU8(cw::kVersion);cw::string(w,m_info.version);cw::string(w,m_info.build_id);
            w.appendU32Le(defs::kDescriptorHash);w.appendU32Le(static_cast<uint32_t>(m_core.capture().capacityBytes()));
            w.appendU16Le(m_info.max_payload);w.appendU8(static_cast<uint8_t>(m_core.instanceCount()));
            w.appendU8((m_core.pendingReboot()?1:0)|(m_core.layoutInvalid()?2:0)|
                (m_core.configLoadedFromNvs()?4:0)|(m_core.safeMode()?8:0));
            w.appendU8(static_cast<uint8_t>(m_core.loopCount()));
            for (size_t i=0;i<m_core.loopCapacity();++i) {
                const auto *l=m_core.loopAt(i); if (!l) { continue; }
                const Ref rate{defs::kLoopTypeId,l->id(),defs::loop::kRateHz};uint32_t hz=0;
                const auto st=m_core.params().get({&rate,1},{&hz,1});if(!st.ok()) { return st; }
                w.appendU8(l->id());cw::string(w,l->name());w.appendU32Le(hz);
                w.appendU8(l->realtime()?1:0);
            }
            return {};
        case defs::SystemOp::BenchCrash:
            if(!m_info.bench_crash) { return bad(Detail::Unsupported); }
            if(m_core.params().armed()) { return Status::fail(Result::Armed,Detail::LockedWhileArmed); }
            m_after=AfterReply::Crash;return {};
        case defs::SystemOp::Save:return m_core.save();
        case defs::SystemOp::Arm:return m_core.arm();
        case defs::SystemOp::Disarm:m_core.disarm();return {};
        case defs::SystemOp::AckFault:return m_core.acknowledgeFault();
        case defs::SystemOp::Revert:return m_core.revert();
        case defs::SystemOp::FactoryReset: {
            auto st=m_core.factoryReset();if(st.ok()) { m_after=AfterReply::Reboot; } return st;
        }
        case defs::SystemOp::ExitSafeMode: {
            if(m_core.params().armed()) { return Status::fail(Result::Armed,Detail::LockedWhileArmed); }
            auto st=m_core.exitSafeMode();if(st.ok()) { m_after=AfterReply::Reboot; } return st;
        }
        case defs::SystemOp::Reboot:
            if(m_core.params().armed()) { return Status::fail(Result::Armed,Detail::LockedWhileArmed); }
            m_after=AfterReply::Reboot;return {};
        default:return bad(Detail::Unsupported); // F4b arm state machine
        }
    }
    case defs::Family::Layout: {
        switch (static_cast<defs::LayoutOp>(h.op)) {
        case defs::LayoutOp::ListInstances: {
            const auto view=r.readU8();const auto start=r.readU16Le();
            if(!r.done() || view>1) { return bad(); }
            const bool pending=view && m_core.pendingLayout().active();
            const size_t total=pending ? m_core.pendingLayout().entries().size() : m_core.configuredCount();
            if(start>total) { return bad(); }
            page(w,static_cast<uint16_t>(total),cw::kEnd,0);
            uint16_t next=start;uint8_t count=0;
            for(;next<total && count<255;++next) {
                const detail::InstanceRecord *rec=nullptr;
                const PendingEntry *entry=nullptr;
                if(pending) { entry=&m_core.pendingLayout().entries()[next]; }
                else { rec=m_core.recordAt(next); }
                const char *name=entry ? entry->label : rec->label;
                if(w.remaining()<6+std::strlen(name)) { break; }
                w.appendU8(entry?entry->type_id:rec->type_id);w.appendU8(entry?entry->instance:rec->instance);
                w.appendU8(entry?entry->loop:rec->loop);
                w.appendU8(entry?0xFF:static_cast<uint8_t>(rec->state));cw::string(w,name);++count;
            }
            if(next<total && count==0) { return bad(Detail::BufferTooSmall); }
            w.patchU8(2,static_cast<uint8_t>(next<total?next:0xFF));
            w.patchU8(3,static_cast<uint8_t>(next<total?next>>8:0xFF));w.patchU8(4,count);return {};
        }
        case defs::LayoutOp::AddInstance: {
            const auto type=r.readU8();auto loop=r.readU8();char name[kLabelBytes+1]{};
            if(!label(r,name)) { return bad(Detail::BadLabel); }
            const auto *t=m_core.registry().find(type);
            if(!t) { return bad(Detail::UnknownType); }
            if(w.remaining()<1) { return bad(Detail::BufferTooSmall); }
            uint8_t instance=0;auto st=m_core.addInstance(type,loop,name,instance);
            if(st.accepted()) { w.appendU8(instance); } return st;
        }
        case defs::LayoutOp::RemoveInstance: {
            const auto type=r.readU8(),instance=r.readU8();if(!r.done()) { return bad(); }
            return m_core.removeInstance(type,instance);
        }
        case defs::LayoutOp::SetLabel: {
            const auto type=r.readU8(),instance=r.readU8();char name[kLabelBytes+1]{};
            if(!label(r,name)) { return bad(Detail::BadLabel); }return m_core.setLabel(type,instance,name);
        }
        case defs::LayoutOp::DiscardPending:if(!r.done()) { return bad(); }return m_core.discardPending();
        default:return bad(Detail::Unsupported);
        }
    }
    case defs::Family::Param: {
        const auto n=r.readU8();const bool set=h.op==static_cast<uint8_t>(defs::ParamOp::Set);
        if(h.op<1 || h.op>3 || !r.ok() || n>kMaxBatchEntries || r.remaining()!=n*(set?8U:4U)) { return bad(); }
        if(w.remaining()<1+4U*n) { return bad(Detail::BufferTooSmall); }
        for(size_t i=0;i<n;++i) {
            m_refs[i]=ref(r);if(set) { m_sets[i]={m_refs[i],r.readU32Le()}; }
        }
        const auto refs=std::span(m_refs.data(),n);auto values=std::span(m_values.data(),n);
        Status st=set?m_core.params().set({m_sets.data(),n},values):
            (h.op==3?m_core.getPending(refs,values):m_core.params().get(refs,values));
        if(st.accepted()) { w.appendU8(n);for(auto v:values) { w.appendU32Le(v); } }return st;
    }
    case defs::Family::Group: {
        auto &groups = m_core.groups();
        const auto op = static_cast<defs::GroupOp>(h.op);
        if (op == defs::GroupOp::List) {
            const auto start = r.readU16Le();
            const auto subs = groups.capacity(GroupKind::Subscription);
            const auto cmds = groups.capacity(GroupKind::Command);
            const auto total = subs + cmds;
            if (!r.done() || start > total) { return bad(); }
            if (w.remaining() < 5) { return bad(Detail::BufferTooSmall); }
            page(w, static_cast<uint16_t>(total), cw::kEnd, 0);
            uint16_t next = start; uint8_t count = 0;
            for (; next < total; ++next) {
                const bool sub = next < subs;
                const auto kind = sub ? GroupKind::Subscription : GroupKind::Command;
                const auto id = static_cast<uint8_t>(sub ? next : next - subs);
                const auto &g = *groups.info(kind, id);
                if (w.remaining() < 8 + 4U * g.count) { break; }
                w.appendU8(sub ? 0 : 1); w.appendU8(id);
                w.appendU8((g.configured ? 1 : 0) | (g.enabled ? 2 : 0));
                w.appendU8(g.loop); w.appendU8(g.epoch); w.appendU16Le(g.divider); w.appendU8(g.count);
                for (size_t i = 0; i < g.count; ++i) {
                    w.appendU8(g.refs[i].type); w.appendU8(g.refs[i].instance); w.appendU16Le(g.refs[i].id);
                }
                ++count;
            }
            if (next < total && count == 0) { return bad(Detail::BufferTooSmall); }
            const auto cursor = next < total ? next : cw::kEnd;
            w.patchU8(2, static_cast<uint8_t>(cursor)); w.patchU8(3, static_cast<uint8_t>(cursor >> 8));
            w.patchU8(4, count);
            return {};
        }
        const auto id = r.readU8();
        const auto kind = op == defs::GroupOp::CmdConfig || op == defs::GroupOp::CmdClear ?
            GroupKind::Command : GroupKind::Subscription;
        if (w.remaining() < 2) { return bad(Detail::BufferTooSmall); }
        Status st;
        switch (op) {
        case defs::GroupOp::SubConfig:
        case defs::GroupOp::CmdConfig: {
            const auto loop = r.readU8();
            const uint16_t divider = kind == GroupKind::Subscription ? r.readU16Le() : 1;
            const auto count = r.readU8();
            if (!r.ok() || count > kMaxGroupEntries || r.remaining() != 4U * count) { return bad(); }
            for (size_t i = 0; i < count; ++i) { m_refs[i] = ref(r); }
            st = groups.configure(kind, id, loop, divider, {m_refs.data(), count}, m_core.params().armed());
            break;
        }
        case defs::GroupOp::SubEnable: {
            const auto enabled = r.readU8();
            if (!r.done() || enabled > 1) { return bad(); }
            st = groups.enable(id, enabled != 0); break;
        }
        case defs::GroupOp::SubClear:
        case defs::GroupOp::CmdClear:
            if (!r.done()) { return bad(); }
            st = groups.clear(kind, id, m_core.params().armed()); break;
        default: return bad(Detail::Unsupported);
        }
        if (st.ok()) {
            w.appendU8(groups.info(kind, id)->epoch);
            w.appendU8(static_cast<uint8_t>((kind == GroupKind::Subscription ? defs::kSubGroupChannelFirst :
                defs::kCmdGroupChannelFirst) + id));
        }
        return st;
    }
    case defs::Family::Describe: {
        const auto op=static_cast<defs::DescribeOp>(h.op);
        uint8_t type=0;uint16_t item_id=0;
        if(op==defs::DescribeOp::GetItems || op==defs::DescribeOp::GetEnum) { type=r.readU8(); }
        if(op==defs::DescribeOp::GetEnum) { item_id=r.readU16Le(); }
        const auto start=r.readU16Le();if(!r.done()) { return bad(); }
        const auto *t=defs::findType(defs::kTypeTable,type);
        const auto *item=t?defs::findItem(*t,item_id):nullptr;
        size_t total=0;
        switch(op) {
        case defs::DescribeOp::GetTypes:total=defs::kTypeTable.size();break;
        case defs::DescribeOp::GetItems:if(!t) { return bad(Detail::UnknownType); }total=t->items.size();break;
        case defs::DescribeOp::GetEnum:if(!item) { return bad(Detail::UnknownRef); }total=item->enum_values.size();break;
        case defs::DescribeOp::GetCodes:total=defs::diag::kCodeTable.size();break;
        default:return bad(Detail::Unsupported);
        }
        if(start>total) { return bad(); }
        page(w,static_cast<uint16_t>(total),cw::kEnd,0);uint16_t next=start;uint8_t count=0;
        for(;next<total && count<255;++next) {
            size_t size=0;
            if(op==defs::DescribeOp::GetTypes) { size=4+std::strlen(defs::kTypeTable[next]->name); }
            if(op==defs::DescribeOp::GetItems) { size=cw::itemSize(t->items[next]); }
            if(op==defs::DescribeOp::GetEnum) { size=3+std::strlen(item->enum_values[next].name); }
            if(op==defs::DescribeOp::GetCodes) { size=8+std::strlen(defs::diag::kCodeTable[next].name); }
            if(size>w.remaining()) { break; }
            if(op==defs::DescribeOp::GetTypes) { const auto &d=*defs::kTypeTable[next];w.appendU8(d.type_id);w.appendU8(d.loops);cw::string(w,d.name); }
            if(op==defs::DescribeOp::GetItems) { cw::item(w,t->items[next]); }
            if(op==defs::DescribeOp::GetEnum) { auto e=item->enum_values[next];w.appendU8(e.value);cw::string(w,e.name); }
            if(op==defs::DescribeOp::GetCodes) {
                const auto &c=defs::diag::kCodeTable[next];w.appendU8(static_cast<uint8_t>(c.source));w.appendU16Le(c.code);
                w.appendU8(static_cast<uint8_t>(c.severity));w.appendU8(static_cast<uint8_t>(c.kind));
                w.appendU8((c.clears?1:0)|(c.blocks_arm?2:0)|(c.disarms?4:0));cw::string(w,c.name);
            }
            ++count;
        }
        if(next<total && count==0) { return bad(Detail::BufferTooSmall); }
        w.patchU8(2,static_cast<uint8_t>(next<total?next:0xFF));w.patchU8(3,static_cast<uint8_t>(next<total?next>>8:0xFF));w.patchU8(4,count);return {};
    }
    case defs::Family::Capture: return capture(h,r,w);
    case defs::Family::Testgen: return testgen(h,r,w);
    case defs::Family::Action: {
        if(h.op!=1) { return bad(Detail::Unsupported); }
        const auto target=ref(r);ActionArgs args;
        for(auto &v:args.values) { v=r.readU32Le(); }
        const auto n=r.readU16Le();if(!r.ok() || n>kActionBlobBytes || r.remaining()!=n) { return bad(); }
        args.blob={};
        // Input remains valid until runAction returns, so no second blob buffer.
        std::array<uint8_t,kActionBlobBytes> blob{};
        for(size_t i=0;i<n;++i) { blob[i]=r.readU8(); }args.blob={blob.data(),n};
        const auto *rec=m_core.findRecord(target.type,target.instance);
        const auto *d=rec?defs::findItem(*rec->desc,target.id):nullptr;
        const size_t max_out=d && (d->blob==defs::BlobDir::Out || d->blob==defs::BlobDir::Both)?kActionBlobBytes:0;
        if(w.remaining()<18+max_out) { return bad(Detail::BufferTooSmall); }
        m_action_out={};const auto st=m_core.runAction(target,args,m_action_out);
        if(st.accepted()) {
            for(auto v:m_action_out.values) { w.appendU32Le(v); }w.appendU16Le(static_cast<uint16_t>(m_action_out.blob_size));
            w.appendBytes(m_action_out.blob.data(),m_action_out.blob_size);
        }return st;
    }
    default:return bad(Detail::Unsupported);
    }
}
}
