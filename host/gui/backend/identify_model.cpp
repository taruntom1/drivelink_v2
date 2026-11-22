#include "identify_model.hpp"
#include "command_model.hpp"
#include "layout_model.hpp"
#include <QCoreApplication>
#include <QLocale>
#include <algorithm>
#include <cmath>
#include <limits>

namespace drivelink::gui {
namespace {
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
QString str(const std::string &s) { return QString::fromStdString(s); }
const ModelType *typeOf(const DeviceSnapshot &s, uint8_t type) {
    const auto it=s.types.find(type);return it==s.types.end()?nullptr:&it->second;
}
QString prefixOf(const std::string &name) {
    const auto dot=name.find('.');
    return dot==std::string::npos?QString():str(name.substr(0,dot));
}
QString number(double v) { return QLocale::c().toString(v,'g',6); }
} // namespace

std::vector<PidGroup> pidGroups(const DeviceSnapshot &s, InstanceKey k) {
    std::vector<PidGroup> result;
    const auto *type=typeOf(s,k.first);
    if(!type) {return result;}
    std::map<QString,PidGroup> groups;
    std::vector<QString> order;
    for(const auto &[id,item]:type->items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Param || !tuneFlagged(d) || d.value_type==ValueType::Enum || d.value_type==ValueType::Bool) {continue;}
        const auto prefix=prefixOf(d.name);
        if(prefix.isEmpty()) {continue;}
        const auto suffix=str(d.name).mid(prefix.size()+1);
        auto &g=groups[prefix];
        if(g.prefix.isEmpty()) {g.prefix=prefix;order.push_back(prefix);}
        if(suffix==QLatin1String("kp")) {g.kp=id;}
        else if(suffix==QLatin1String("ki")) {g.ki=id;}
        else if(suffix==QLatin1String("kd")) {g.kd=id;}
    }
    for(const auto &prefix:order) {
        const auto &g=groups[prefix];
        if(g.kp && g.ki) {result.push_back(g);}
    }
    return result;
}
std::optional<PidGroup> groupFor(const std::vector<PidGroup> &groups, const QString &signalName) {
    const PidGroup *best=nullptr;
    for(const auto &g:groups) {
        if(signalName.startsWith(g.prefix) && (!best || g.prefix.size()>best->prefix.size())) {best=&g;}
    }
    return best?std::optional<PidGroup>(*best):std::nullopt;
}

std::vector<ModeChoice> modeChoices(const DeviceSnapshot &s, InstanceKey k) {
    std::vector<ModeChoice> result;
    const auto *type=typeOf(s,k.first);
    if(!type) {return result;}
    for(const auto &[id,item]:type->items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Param || !tuneFlagged(d) || d.value_type!=ValueType::Enum) {continue;}
        for(const auto &e:item.enums) {
            result.push_back({id,e.value,str(e.name),QStringLiteral("%1: %2").arg(str(d.name),str(e.name))});
        }
    }
    return result;
}
std::optional<ModeChoice> defaultMode(const DeviceSnapshot &s, InstanceKey k) {
    for(const auto &c:modeChoices(s,k)) {
        if(c.name==QLatin1String("open_loop")) {return c;}
    }
    return std::nullopt;
}
std::optional<uint32_t> enumValue(const DeviceSnapshot &s, InstanceKey k, uint16_t item) {
    const auto *entry=configuredEntry(s,k);
    if(!entry) {return std::nullopt;}
    const auto it=entry->params.find(item);
    if(it==entry->params.end()) {return std::nullopt;}
    return valueBits(it->second);
}

std::vector<TuneTarget> stepInputs(const TuneController &c) {
    std::vector<TuneTarget> result;
    for(const auto &t:c.targets) {
        if(commandable(t.descriptor)) {result.push_back(t);}
    }
    return result;
}
std::optional<TuneTarget> defaultStepInput(const TuneController &c) {
    const auto inputs=stepInputs(c);
    for(const auto &t:inputs) {
        if(t.descriptor.unit.empty()) {return t;}
    }
    return inputs.empty()?std::nullopt:std::optional<TuneTarget>(inputs.front());
}
std::vector<Ref> responseCandidates(const DeviceSnapshot &s, InstanceKey k) {
    std::vector<Ref> result;
    const auto *type=typeOf(s,k.first);
    if(!type) {return result;}
    const auto groups=pidGroups(s,k);
    for(const auto &[id,item]:type->items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Signal || d.value_type!=ValueType::F32 || d.unit.empty()) {continue;}
        if(groupFor(groups,str(d.name))) {result.push_back({k.first,k.second,id});}
    }
    return result;
}

TestSignal identifySignal(const IdentifyStep &step) {
    // Square: +1 for the first half-period, −1 for the second. Even from 0 the
    // first half is the baseline: a capture armed just before START may hold
    // no samples before the trigger.
    TestSignal s;
    s.target=step.input;s.repeat=1;
    s.waveform=Waveform::Square;
    s.offset=(step.from+step.to)/2;s.amplitude=(step.from-step.to)/2;
    s.f0=step.seconds>0?1.0/(2*step.seconds):0;s.duration_s=2*step.seconds;
    return s;
}
QString identifyProblem(const IdentifyStep &step, double rate, const ItemDescriptor &input) {
    for(const double v:{step.from,step.to,step.seconds}) {if(!std::isfinite(v)) {return tr("Every value must be a number");}}
    if(step.to==step.from) {return tr("The step must change the input");}
    if(!(rate>0)) {return tr("The loop rate is unknown");}
    if(step.seconds*rate<20) {return tr("Capture at least %1 s (20 loop periods)").arg(number(20/rate));}
    if(step.seconds>60) {return tr("Capture at most 60 s");}
    if(!commandable(input)) {return tr("%1 cannot return to 0 after the step: %2").arg(str(input.name),commandRefusal(input));}
    return testSignalProblem(identifySignal(step),rate,input);
}
CaptureSetup identifyCapture(const IdentifyStep &step, const std::vector<Ref> &candidates, double rate,
                             uint32_t capacity, uint32_t maxRefs) {
    CaptureSetup c;
    c.refs.push_back(step.input);
    for(const auto &r:candidates) {
        if(maxRefs && c.refs.size()>=maxRefs) {break;}
        if(r!=step.input) {c.refs.push_back(r);}
    }
    const double seconds=signalSeconds(identifySignal(step));
    c.divider=1;
    c=resizeCapture(c,rate,seconds);
    while(captureBytes(c)>capacity && c.divider<10000) {
        c.divider=uint16_t(c.divider<5?c.divider+1:c.divider*2);
        c=resizeCapture(c,rate,seconds);
    }
    return c;
}

IdentifyResult makeIdentify(const DeviceSnapshot &s, RunRecord run, const IdentifyStep &step,
                            std::optional<ModeChoice> mode, double rate) {
    IdentifyResult r;
    r.step=step;r.mode=std::move(mode);r.loopPeriodS=rate>0?1/rate:0;
    if(const auto *entry=anyEntry(s,run.controller)) {r.controllerLabel=str(entry->instance.label);}
    const auto input=std::find(run.capture.refs.begin(),run.capture.refs.end(),step.input);
    const auto groups=pidGroups(s,run.controller);
    // The step's samples: up to where the input leaves `to` after stepping to
    // it (the return to 0 after the signal), or the Stop or disarm. Sample
    // times count samples, so on irregular ticks (desktop vdev) the signal's
    // nominal length is not a cut that can be trusted.
    double end=signalSeconds(run.signal);
    if(run.endedUs) {end=std::min(end,double(*run.endedUs)/1e6);}
    if(input!=run.capture.refs.end() && size_t(input-run.capture.refs.begin())<run.series.size()) {
        const auto &u=run.series[size_t(input-run.capture.refs.begin())];
        const auto isTo=[&](double v) {return std::fabs(v-step.to)<=1e-6*std::max(1.0,std::fabs(step.to));};
        std::optional<size_t> into;
        for(size_t i=0;i<u.size();++i) {
            if(u.time(i)<0) {continue;}
            if(!into) {
                if(isTo(u.value(i)) && i>0 && !isTo(u.value(i-1))) {into=i;}
            } else if(!isTo(u.value(i))) {
                end=std::min(end,double(u.time(i-1))/1e6);break;
            }
        }
        r.stepEndUs=int64_t(end*1e6);
        for(size_t j=0;j<run.capture.refs.size();++j) {
            if(run.capture.refs[j]==step.input || j>=run.series.size()) {continue;}
            const auto &y=run.series[j];
            std::vector<double> t,uu,yy;
            const size_t n=std::min(u.size(),y.size());
            t.reserve(n);uu.reserve(n);yy.reserve(n);
            for(size_t i=0;i<n;++i) {
                const double ts=double(y.time(i))/1e6;
                if(ts>end) {break;}
                t.push_back(ts);uu.push_back(u.value(i));yy.push_back(y.value(i));
            }
            ResponseFit f;
            f.index=int(j);f.name=run.names[j];f.unit=run.units[j];
            f.group=groupFor(groups,f.name);
            f.fit=fitFopdt(t,uu,yy);
            r.fits.push_back(std::move(f));
        }
    }
    // The settled response with the smallest error; else the first with a model.
    for(size_t i=0;i<r.fits.size();++i) {
        const auto &f=r.fits[i].fit;
        if(!f.gain || !f.settled || !f.error_pct) {continue;}
        if(r.response<0 || *f.error_pct<*r.fits[size_t(r.response)].fit.error_pct) {r.response=int(i);}
    }
    if(r.response<0) {
        for(size_t i=0;i<r.fits.size();++i) {if(r.fits[i].fit.gain) {r.response=int(i);break;}}
    }
    if(r.response<0 && !r.fits.empty()) {r.response=0;}
    r.run=std::move(run);
    return r;
}

bool runsOpenLoop(const DeviceSnapshot &s, InstanceKey k, const std::optional<ModeChoice> &mode, const ItemDescriptor &input) {
    if(mode) {return mode->name==QLatin1String("open_loop");}
    const auto choices=modeChoices(s,k);
    if(choices.empty()) {return input.unit.empty();}
    for(const auto &c:choices) {
        if(c.name==QLatin1String("open_loop") && enumValue(s,k,c.item)==c.value) {return true;}
    }
    return false;
}

Suggestion suggestGains(const DeviceSnapshot &s, InstanceKey k, const ResponseFit &f, TuneSpeed speed, double period,
                        bool openLoop) {
    Suggestion out;
    // A closed-loop step (velocity_sp in velocity mode) fits the closed loop,
    // not the plant: its K and T would give gains in the wrong units.
    if(!openLoop) {out.problem=tr("Suggestions need an open-loop step: hold the mode value open_loop during the step");return out;}
    if(!f.group) {out.problem=tr("%1 has no PID gains (<prefix>.kp and .ki) to suggest").arg(f.name);return out;}
    out.pi=simcPi(f.fit,speed,period);
    if(!out.pi.problem.empty()) {out.problem=str(out.pi.problem);return out;}
    const auto *type=typeOf(s,k.first);
    const auto *entry=configuredEntry(s,k);
    if(!type || !entry) {out.problem=tr("The controller is not running");return out;}
    const auto add=[&](uint16_t id,double value) {
        const auto it=type->items.find(id);
        if(it==type->items.end()) {return;}
        const auto &d=it->second.descriptor;
        SuggestedGain g;
        g.item=id;g.name=str(d.name);
        if(const auto p=entry->params.find(id);p!=entry->params.end()) {g.current=bitsToNumber(d.value_type,valueBits(p->second));}
        double v=roundGain(value);
        const double lo=d.has_min?bitsToNumber(d.value_type,d.min_bits):-std::numeric_limits<float>::max();
        const double hi=d.has_max?bitsToNumber(d.value_type,d.max_bits):std::numeric_limits<float>::max();
        if(v<lo || v>hi) {v=std::clamp(v,lo,hi);g.clamped=true;}
        if(d.value_type!=ValueType::F32) {v=std::round(v);}
        g.suggested=v;
        out.gains.push_back(g);
    };
    add(f.group->kp,out.pi.kp);
    add(f.group->ki,out.pi.ki);
    if(f.group->kd) {add(*f.group->kd,0);}
    if(out.gains.size()<2) {out.problem=tr("The gains are not in the controller's descriptors");}
    return out;
}

QString qualityText(FitQuality q) {
    switch(q) {
    case FitQuality::Good:return tr("good fit");
    case FitQuality::Fair:return tr("fair fit");
    case FitQuality::Poor:return tr("poor fit");
    }
    return {};
}
QString gainUnit(const QString &response, const QString &input) {
    return tr("%1 per %2").arg(response.isEmpty()?tr("unit"):response,input.isEmpty()?tr("unit"):input);
}
} // namespace drivelink::gui
