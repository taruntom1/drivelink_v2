#include "tune_model.hpp"
#include "command_model.hpp"
#include "layout_model.hpp"
#include <drivelink/protocol/defs.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <QCoreApplication>
#include <QLocale>
#include <algorithm>
#include <cmath>
#include <set>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
QString str(const std::string &s) { return QString::fromStdString(s); }
const ModelType *typeOf(const DeviceSnapshot &s, uint8_t type) {
    const auto it=s.types.find(type);return it==s.types.end()?nullptr:&it->second;
}
bool loopRunning(const DeviceSnapshot &s, uint8_t loop) {
    return std::any_of(s.info.loops.begin(),s.info.loops.end(),[&](const LoopInfo &l){return l.id==loop;});
}
bool numeric(ValueType t) {
    return t==ValueType::U8 || t==ValueType::U16 || t==ValueType::U32 || t==ValueType::I32 || t==ValueType::F32;
}
QString prefixOf(const std::string &name) {
    const auto dot=name.find('.');
    return dot==std::string::npos?QString():str(name.substr(0,dot));
}
double rangeMin(const ItemDescriptor &d) {
    return d.has_min?bitsToNumber(d.value_type,d.min_bits):(d.value_type==ValueType::F32?-std::numeric_limits<float>::max():
           d.value_type==ValueType::I32?double(INT32_MIN):0.0);
}
double rangeMax(const ItemDescriptor &d) {
    if(d.has_max) {return bitsToNumber(d.value_type,d.max_bits);}
    switch(d.value_type) {
    case ValueType::U8:return 255;case ValueType::U16:return 65535;case ValueType::U32:return 4294967295.0;
    case ValueType::I32:return double(INT32_MAX);default:return std::numeric_limits<float>::max();
    }
}
} // namespace

bool tuneFlagged(const ItemDescriptor &d) { return (d.flags&uint16_t(defs::ItemFlag::Tune))!=0; }
bool testgenFlagged(const ItemDescriptor &d) { return (d.flags&uint16_t(defs::ItemFlag::Testgen))!=0; }

std::vector<TuneController> tuneControllers(const DeviceSnapshot &s) {
    std::vector<TuneController> result;
    for(const auto &[k,entry]:s.configured) {
        if(k.first==defs::kLoopTypeId || entry.instance.state!=0 || !loopRunning(s,entry.instance.loop)) {continue;}
        const auto *type=typeOf(s,k.first);
        if(!type) {continue;}
        TuneController c{k,str(entry.instance.label),str(type->descriptor.name),entry.instance.loop,{}};
        for(const auto &[id,item]:type->items) {
            const auto &d=item.descriptor;
            if(d.kind==ItemKind::Input && d.value_type==ValueType::F32 && testgenFlagged(d)) {c.targets.push_back({{k.first,k.second,id},d});}
        }
        if(!c.targets.empty()) {result.push_back(std::move(c));}
    }
    return result;
}

GainLayout gainLayout(const DeviceSnapshot &s, InstanceKey k) {
    GainLayout layout;
    const auto *type=typeOf(s,k.first);
    const auto *entry=configuredEntry(s,k);
    if(!type || !entry) {return layout;}
    std::vector<QString> names,selected;
    std::map<QString,GainGroup> groups;
    std::vector<QString> order;
    for(const auto &[id,item]:type->items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Param || !tuneFlagged(d)) {continue;}
        if(d.value_type==ValueType::Enum) {
            layout.enums.push_back(id);
            const auto value=entry->params.find(id);
            for(const auto &e:item.enums) {
                names.push_back(str(e.name));
                if(value!=entry->params.end() && valueBits(value->second)==e.value) {selected.push_back(str(e.name));}
            }
        } else if(numeric(d.value_type)) {
            const auto prefix=prefixOf(d.name);
            if(!groups.contains(prefix)) {order.push_back(prefix);groups[prefix].prefix=prefix;}
            groups[prefix].items.push_back(id);
        }
    }
    const auto startsAny=[](const std::vector<QString> &list,const QString &prefix) {
        return !prefix.isEmpty() && std::any_of(list.begin(),list.end(),[&](const QString &n){return n.startsWith(prefix);});
    };
    std::vector<GainGroup> first,open,closed;
    for(const auto &prefix:order) {
        auto g=groups[prefix];
        g.selected=startsAny(selected,prefix);
        g.open=g.selected || !startsAny(names,prefix);
        (g.selected?first:g.open?open:closed).push_back(std::move(g));
    }
    for(auto *list:{&first,&open,&closed}) {for(auto &g:*list) {layout.groups.push_back(std::move(g));}}
    return layout;
}

std::optional<GainWindow> gainWindow(double value, double min, double max) {
    if(!std::isfinite(value) || value==0) {return std::nullopt;}
    // The magnitudes the range allows on the value's side of 0.
    const double sideLo=value>0?std::max(min,0.0):std::max(-max,0.0);
    const double sideHi=value>0?max:-min;
    const double m=std::fabs(value);
    GainWindow w{std::max(m/4,sideLo),std::min(m*4,sideHi)};
    if(w.lo<=0) {w.lo=m/4;}
    if(!(w.hi>w.lo)) {return std::nullopt;}
    return w;
}
std::optional<GainWindow> typedWindow(double lo, double hi, double min, double max, double value) {
    if(!std::isfinite(lo) || !std::isfinite(hi) || value==0) {return std::nullopt;}
    const bool positive=value>0;
    const double sideHi=positive?max:-min,sideLo=positive?std::max(min,0.0):std::max(-max,0.0);
    lo=std::fabs(lo);hi=std::fabs(hi);
    if(lo>hi) {std::swap(lo,hi);}
    lo=std::max(lo,sideLo);hi=std::min(hi,sideHi);
    if(!(lo>0) || !(hi>lo)) {return std::nullopt;}
    return GainWindow{lo,hi};
}
double logPosition(double magnitude, GainWindow w) {
    if(!(magnitude>0)) {return 0;}
    return std::clamp(std::log(magnitude/w.lo)/std::log(w.hi/w.lo),0.0,1.0);
}
double logMagnitude(double position, GainWindow w) {
    return w.lo*std::pow(w.hi/w.lo,std::clamp(position,0.0,1.0));
}
double roundGain(double value) {
    if(!std::isfinite(value) || value==0) {return value;}
    const double digits=std::floor(std::log10(std::fabs(value)));
    const double scale=std::pow(10.0,2-digits);
    return std::round(value*scale)/scale;
}
double stepGain(double value, double factor, double min, double max) {
    if(value==0 || !std::isfinite(value)) {return value;}
    double next=roundGain(value*factor);
    // A 1 % step on a 3-digit value may round back to it: move one digit.
    if(next==value && factor!=1) {
        const double unit=std::pow(10.0,std::floor(std::log10(std::fabs(value)))-2);
        next=value+(factor>1?1:-1)*(value>0?unit:-unit);
    }
    return std::clamp(next,min,max);
}
QString gainText(double value) {
    if(!std::isfinite(value)) {return QStringLiteral("—");}
    if(value==0) {return QStringLiteral("0.0");}
    const double digits=std::floor(std::log10(std::fabs(value)));
    const int decimals=int(std::clamp(2-digits,1.0,9.0));
    auto text=QLocale::c().toString(roundGain(value),'f',decimals);
    // Trailing zeros beyond the first decimal carry nothing (0.850 -> 0.85).
    while(text.contains('.') && text.endsWith('0') && text.indexOf('.')<text.size()-2) {text.chop(1);}
    return text;
}

QString waveformName(Waveform w) {
    switch(w) {
    case Waveform::Step:return tr("step");case Waveform::Square:return tr("square");case Waveform::Ramp:return tr("ramp");
    case Waveform::Sine:return tr("sine");case Waveform::ChirpLinear:return tr("chirp");case Waveform::ChirpLog:return tr("log chirp");
    }
    return {};
}
bool waveformUsesFrequency(Waveform w) { return w!=Waveform::Step && w!=Waveform::Ramp; }
bool waveformIsChirp(Waveform w) { return w==Waveform::ChirpLinear || w==Waveform::ChirpLog; }

QString testSignalProblem(const TestSignal &s, double rate, const ItemDescriptor &target) {
    for(const double v:{s.offset,s.amplitude,s.f0,s.f1,s.duration_s}) {if(!std::isfinite(v)) {return tr("Every value must be a number");}}
    if(s.duration_s*1000<1 || s.duration_s*1000>double(UINT32_MAX)) {return tr("Duration must be at least 1 ms");}
    if(s.repeat<1) {return tr("Repeat at least once");}
    if(s.repeat>1000) {return tr("Repeat at most 1000 times");}
    const double nyquist=rate/2;
    if(waveformUsesFrequency(s.waveform)) {
        if(!(rate>0)) {return tr("The loop rate is unknown");}
        if(!(s.f0>0) || s.f0>=nyquist) {return tr("Frequency must be above 0 and below %1 Hz (half the loop rate)").arg(QLocale::c().toString(nyquist,'g',6));}
        if(waveformIsChirp(s.waveform) && (!(s.f1>0) || s.f1>=nyquist)) {
            return tr("End frequency must be above 0 and below %1 Hz (half the loop rate)").arg(QLocale::c().toString(nyquist,'g',6));
        }
    }
    // Every output stays inside the target's range: the device would clamp.
    double lo=s.offset,hi=s.offset;
    switch(s.waveform) {
    case Waveform::Step:case Waveform::Ramp:lo=std::min(s.offset,s.offset+s.amplitude);hi=std::max(s.offset,s.offset+s.amplitude);break;
    default:lo=s.offset-std::fabs(s.amplitude);hi=s.offset+std::fabs(s.amplitude);break;
    }
    const double min=rangeMin(target),max=rangeMax(target);
    if(lo<min || hi>max) {
        return tr("The signal leaves %1's range (%2 to %3)").arg(str(target.name),QLocale::c().toString(min,'g',6),QLocale::c().toString(max,'g',6));
    }
    if(s.amplitude==0) {return tr("Amplitude is 0: the signal would not move");}
    return {};
}
TestgenConfig testgenConfig(const TestSignal &s, uint8_t loop) {
    TestgenConfig c;
    c.loop=loop;c.target=s.target;c.waveform=uint8_t(s.waveform);
    c.offset=float(s.offset);c.amplitude=float(s.amplitude);
    c.frequency_start=waveformUsesFrequency(s.waveform)?float(s.f0):0.0f;
    c.frequency_end=waveformIsChirp(s.waveform)?float(s.f1):0.0f;
    c.duration_ms=uint32_t(std::llround(s.duration_s*1000));c.repeat=std::max<uint32_t>(1,s.repeat);
    return c;
}

QString captureProblem(const DeviceSnapshot &s, const CaptureSetup &c, uint8_t loop, uint32_t capacity, uint32_t maxRefs) {
    if(c.refs.empty()) {return tr("Capture at least one signal");}
    if(maxRefs && c.refs.size()>maxRefs) {return tr("At most %1 signals per capture").arg(maxRefs);}
    std::set<std::tuple<uint8_t,uint8_t,uint16_t>> seen;
    for(const auto &r:c.refs) {
        if(!seen.insert({r.type,r.instance,r.item}).second) {return tr("A signal is listed twice");}
        const auto signal=monitorSignal(s,r);
        if(!signal) {return tr("A captured signal is not running");}
        if(signal->loop!=loop) {return tr("%1 runs in another loop; a capture records one loop").arg(signal->name);}
    }
    if(c.divider<1) {return tr("The rate divider must be at least 1");}
    if(c.samples<2) {return tr("Capture at least 2 samples");}
    if(c.pre>=c.samples) {return tr("Samples before the trigger must be fewer than all samples");}
    if(captureBytes(c)>capacity) {return tr("Needs %1; the device has %2 for captures").arg(bytesText(captureBytes(c)),bytesText(capacity));}
    return {};
}
CaptureSetup resizeCapture(CaptureSetup c, double rate, double seconds) {
    const double hz=rate/std::max<uint16_t>(1,c.divider);
    const auto post=uint32_t(std::max(2.0,std::ceil(seconds*hz)));
    c.pre=std::max<uint32_t>(1,uint32_t(std::lround(post/15.0)));
    c.samples=post+c.pre;
    return c;
}
CaptureSetup defaultCapture(const DeviceSnapshot &s, const TuneController &controller, const TuneTarget &target,
                            double rate, double seconds, uint32_t capacity) {
    CaptureSetup c;
    c.refs.push_back(target.ref);
    if(const auto *type=typeOf(s,controller.key.first)) {
        for(const auto &[id,item]:type->items) {
            const auto &d=item.descriptor;
            if(d.kind==ItemKind::Signal && !d.unit.empty() && d.unit==target.descriptor.unit && d.value_type==ValueType::F32) {
                c.refs.push_back({controller.key.first,controller.key.second,id});break;
            }
        }
    }
    c.divider=1;
    c=resizeCapture(c,rate,seconds);
    while(captureBytes(c)>capacity && c.divider<10000) {
        c.divider=uint16_t(c.divider<5?c.divider+1:c.divider*2);
        c=resizeCapture(c,rate,seconds);
    }
    return c;
}
QString bytesText(uint64_t bytes) {
    if(bytes<1024) {return tr("%1 B").arg(bytes);}
    if(bytes<1024*1024) {return tr("%1 KB").arg(QLocale::c().toString(double(bytes)/1024,'f',bytes<10*1024?1:0));}
    return tr("%1 MB").arg(QLocale::c().toString(double(bytes)/1024/1024,'f',bytes<10*1024*1024?1:0));
}

RunRecord makeRun(const DeviceSnapshot &s, const CaptureData &data, const CaptureSetup &capture, const TestSignal &signal) {
    RunRecord r;
    r.signal=signal;r.capture=capture;r.irregular=data.status.irregular;
    const auto &st=data.status;
    const size_t n=st.refs.size();
    const int64_t step=int64_t(st.period_us)*std::max<uint16_t>(1,st.divider);
    const int64_t trigger=st.trigger_index?int64_t(*st.trigger_index):0;
    r.series.reserve(n);
    for(size_t j=0;j<n;++j) {
        const auto *type=typeOf(s,st.refs[j].type);
        const ModelItem *item=nullptr;
        if(type) {if(const auto it=type->items.find(st.refs[j].item);it!=type->items.end()) {item=&it->second;}}
        r.names.push_back(item?str(item->descriptor.name):QStringLiteral("?"));
        r.units.push_back(item?str(item->descriptor.unit):QString());
        SampleRing ring(std::max<size_t>(16,data.data.size()));
        if(st.trigger_index) {
            for(size_t i=0;i<data.data.size();++i) {
                if(j>=data.data[i].size()) {continue;}
                ring.push((int64_t(i)-trigger)*step,item?sampleValue(item->descriptor.value_type,data.data[i][j]):double(data.data[i][j]));
            }
        }
        r.series.push_back(std::move(ring));
    }
    // The capture's refs are the device's (STATUS); keep the setup's order.
    r.capture.refs=st.refs;
    r.startUs=-trigger*step;
    r.endUs=data.data.empty()?0:(int64_t(data.data.size())-1-trigger)*step;
    r.response=defaultResponse(r);
    return r;
}
int defaultResponse(const RunRecord &r) {
    const auto target=std::find(r.capture.refs.begin(),r.capture.refs.end(),r.signal.target);
    if(target==r.capture.refs.end()) {return -1;}
    const auto unit=r.units[size_t(target-r.capture.refs.begin())];
    for(size_t i=0;i<r.capture.refs.size();++i) {
        if(r.capture.refs[i]!=r.signal.target && !unit.isEmpty() && r.units[i]==unit) {return int(i);}
    }
    return -1;
}
StepMetrics runMetrics(const RunRecord &r) {
    StepMetrics m;
    if(r.signal.waveform!=Waveform::Step) {m.problem=tr("Metrics are for step runs").toStdString();return m;}
    if(r.response<0 || size_t(r.response)>=r.series.size()) {
        m.problem=tr("No captured signal in the target's unit").toStdString();return m;
    }
    double from=0;
    const auto target=std::find(r.capture.refs.begin(),r.capture.refs.end(),r.signal.target);
    if(target!=r.capture.refs.end()) {
        const auto &ring=r.series[size_t(target-r.capture.refs.begin())];
        const auto first=ring.lowerBound(0);
        if(first>0) {from=ring.value(first-1);}
    }
    const auto &ring=r.series[size_t(r.response)];
    std::vector<double> t,y;
    t.reserve(ring.size());y.reserve(ring.size());
    for(size_t i=0;i<ring.size();++i) {t.push_back(double(ring.time(i))/1e6);y.push_back(ring.value(i));}
    StepOptions options;
    options.end_time=signalSeconds(r.signal);
    if(r.endedUs) {options.end_time=std::min(options.end_time,double(*r.endedUs)/1e6);}
    return stepMetrics(t,y,0,from,r.signal.offset+r.signal.amplitude,options);
}
QString changedGains(const DeviceSnapshot &s, InstanceKey k, const std::map<uint16_t,double> &now,
                     const std::map<uint16_t,double> *previous) {
    QStringList parts;
    const auto *type=typeOf(s,k.first);
    for(const auto &[id,value]:now) {
        if(previous) {
            const auto it=previous->find(id);
            if(it!=previous->end() && it->second==value) {continue;}
        }
        QString name=QString::number(id);
        if(type) {if(const auto it=type->items.find(id);it!=type->items.end()) {name=str(it->second.descriptor.name);}}
        parts<<QStringLiteral("%1 %2").arg(name,gainText(value));
    }
    if(parts.isEmpty()) {return previous?tr("no gain change"):QString();}
    return parts.join(QStringLiteral(", "));
}

std::vector<OverlayEntry> overlay(const std::vector<RunRecord> &runs) {
    std::vector<OverlayEntry> result;
    const size_t n=runs.size();
    for(size_t i=0;i<n;++i) {
        const bool recent=i+3>=n;
        if(!recent && !runs[i].pinned) {continue;}
        result.push_back({i,recent && i+1!=n && !runs[i].pinned});
    }
    return result;
}
void trimRuns(std::vector<RunRecord> &runs) {
    while(runs.size()>kMaxRuns) {
        auto it=std::find_if(runs.begin(),runs.end(),[](const RunRecord &r){return !r.pinned;});
        runs.erase(it==runs.end()?runs.begin():it);
    }
}
QString metricText(const std::optional<double> &v, const QString &unit, double scale, int decimals) {
    if(!v || !std::isfinite(*v)) {return QStringLiteral("—");}
    const auto number=QLocale::c().toString(*v*scale,'f',decimals);
    return unit.isEmpty()?number:number+QChar(' ')+unit;
}
} // namespace drivelink::gui
