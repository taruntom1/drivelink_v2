#include "command_model.hpp"
#include <QCoreApplication>
#include <bit>
#include <cmath>
#include <limits>

namespace drivelink::gui {
namespace {
bool integral(ValueType t) {return t==ValueType::I32 || t==ValueType::U8 || t==ValueType::U16 || t==ValueType::U32;}
double roundTo(double value,int decimals) {
    const double scale=std::pow(10.0,decimals);
    const double r=std::round(value*scale)/scale;
    return r==0?0:r; // no -0
}
QString tr(const char *text) {return QCoreApplication::translate("CommandModel",text);}
}

QString commandRefusal(const ItemDescriptor &d) {
    if(d.kind!=ItemKind::Input) {return tr("Not an input");}
    if(d.value_type!=ValueType::F32 && !integral(d.value_type)) {return tr("Not a numeric input");}
    if(!d.has_min || !d.has_max) {return tr("Its descriptor gives no finite range");}
    const double lower=bitsToNumber(d.value_type,d.min_bits),upper=bitsToNumber(d.value_type,d.max_bits);
    if(!std::isfinite(lower) || !std::isfinite(upper)) {return tr("Its descriptor gives no finite range");}
    if(lower>0 || upper<0) {return tr("Its range does not include 0");}
    if(lower>=upper) {return tr("Its range allows only 0");}
    // The card shows at most 4 decimals: a finer range could only send 0.
    if(std::max(std::abs(lower),std::abs(upper))<1e-4) {return tr("Its range is finer than the card shows");}
    return {};
}
double bitsToNumber(ValueType type,uint32_t bits) {
    switch(type) {
    case ValueType::F32:return double(std::bit_cast<float>(bits));
    case ValueType::I32:return double(std::bit_cast<int32_t>(bits));
    case ValueType::U8:case ValueType::Enum:case ValueType::Bool:return double(bits&0xFF);
    case ValueType::U16:return double(bits&0xFFFF);
    case ValueType::U32:return double(bits);
    default:return std::numeric_limits<double>::quiet_NaN();
    }
}
uint32_t numberToBits(ValueType type,double value) {
    switch(type) {
    case ValueType::F32:return std::bit_cast<uint32_t>(float(value));
    case ValueType::I32:return std::bit_cast<uint32_t>(int32_t(value));
    default:return uint32_t(value);
    }
}

void CommandModel::applyLimit(Input &in,double limit) {
    const bool ints=integral(in.descriptor.value_type);
    limit=std::clamp(limit,std::min(ints?1.0:1e-4,in.bound),in.bound);
    in.decimals=ints?0:limit>=100?0:limit>=10?1:limit>=0.1?2:limit>=0.01?3:4;
    in.step=std::pow(10.0,-in.decimals);
    // The limit is shown at the same resolution as the values.
    in.limit=std::clamp(roundTo(limit,in.decimals),in.step,in.bound);
}
double CommandModel::defaultLimit(const Input &in) {
    const double base=in.upper>0?in.upper:-in.lower;
    const double quarter=base*0.25;
    return integral(in.descriptor.value_type)?std::max(1.0,std::round(quarter)):quarter;
}
void CommandModel::configure(const std::vector<CommandGroupSpec> &groups) {
    clear();
    for(const auto &g:groups) {
        Group group;group.id=g.id;
        for(const auto &spec:g.inputs) {
            if(!commandable(spec.descriptor)) {throw Error(ErrorKind::Usage,"a value group holds an input outside the hold rule");}
            Input in;in.ref=spec.ref;in.descriptor=spec.descriptor;in.group=m_groups.size();
            in.lower=bitsToNumber(spec.descriptor.value_type,spec.descriptor.min_bits);
            in.upper=bitsToNumber(spec.descriptor.value_type,spec.descriptor.max_bits);
            in.bound=std::max(std::abs(in.lower),std::abs(in.upper));
            applyLimit(in,defaultLimit(in));
            group.inputs.push_back(m_inputs.size());
            m_inputs.push_back(std::move(in));
        }
        m_groups.push_back(std::move(group));
    }
}
void CommandModel::clear() {m_groups.clear();m_inputs.clear();}
int CommandModel::find(const Ref &ref) const {
    for(size_t i=0;i<m_inputs.size();++i) {if(m_inputs[i].ref==ref) {return int(i);}}
    return -1;
}
void CommandModel::resetValues() {
    // In-flight writes stay counted: their replies still end them.
    for(auto &in:m_inputs) {in.value=0;in.latched=false;in.held=false;in.ramping=false;}
    for(auto &g:m_groups) {g.dirty=false;}
}
void CommandModel::setEnabled(bool on) {
    if(on==m_enabled) {return;}
    m_enabled=on;resetValues();
}
double CommandModel::clampValue(const Input &in,double value) const {
    if(!std::isfinite(value)) {return 0;}
    const double lo=in.from(),hi=in.to();
    value=std::clamp(value,lo,hi);
    double q=roundTo(std::round(value/in.step)*in.step,in.decimals);
    // Rounding may step past a span end that is not on the step.
    if(q>hi) {q=roundTo(q-in.step,in.decimals);}
    if(q<lo) {q=roundTo(q+in.step,in.decimals);}
    return std::clamp(q,lo,hi)==q?q:0;
}
void CommandModel::setValue(Input &in,double value) {
    if(value==in.value) {return;}
    in.value=value;
    if(m_enabled) {m_groups[in.group].dirty=true;}
}
void CommandModel::setLimit(int index,double limit) {
    if(index<0 || size_t(index)>=m_inputs.size() || !std::isfinite(limit)) {return;}
    auto &in=m_inputs[size_t(index)];
    applyLimit(in,limit);
    // On the new step, toward zero: a limit change never raises a value.
    auto towardZero=[&](double v) {
        double q=clampValue(in,v);
        if(std::abs(q)>std::abs(v)) {q=roundTo(q-(q>0?in.step:-in.step),in.decimals);}
        return q;
    };
    if(in.ramping) {in.rampFrom=towardZero(in.rampFrom);}
    setValue(in,towardZero(in.value));
}
void CommandModel::setShown(int index,bool shown) {
    if(index>=0 && size_t(index)<m_inputs.size()) {m_inputs[size_t(index)].shown=shown;}
}
bool CommandModel::press(int index) {
    if(!m_enabled || index<0 || size_t(index)>=m_inputs.size() || !m_inputs[size_t(index)].shown) {return false;}
    auto &in=m_inputs[size_t(index)];
    if(in.writes>0) {return false;}
    // Pressing stops a ramp where it is: that value is the one shown.
    in.held=true;in.ramping=false;
    return true;
}
bool CommandModel::drag(int index,double value) {
    if(!m_enabled || index<0 || size_t(index)>=m_inputs.size()) {return false;}
    auto &in=m_inputs[size_t(index)];
    if(!in.held) {return false;}
    setValue(in,clampValue(in,value));
    return true;
}
void CommandModel::release(int index,int64_t now) {
    if(index<0 || size_t(index)>=m_inputs.size()) {return;}
    auto &in=m_inputs[size_t(index)];
    if(!in.held) {return;}
    in.held=false;
    if(!in.latched && in.value!=0 && m_enabled) {in.ramping=true;in.rampFrom=in.value;in.rampStart=now;}
}
void CommandModel::releaseHeld(int64_t now) {
    for(size_t i=0;i<m_inputs.size();++i) {if(m_inputs[i].held) {release(int(i),now);}}
}
void CommandModel::setLatched(int index,bool on,int64_t now) {
    if(index<0 || size_t(index)>=m_inputs.size()) {return;}
    auto &in=m_inputs[size_t(index)];
    if(!m_enabled || (on && in.writes>0)) {in.latched=false;return;}
    in.latched=on;
    if(on) {in.ramping=false;return;}
    if(!in.held && in.value!=0) {in.ramping=true;in.rampFrom=in.value;in.rampStart=now;}
}
std::optional<double> CommandModel::type(int index,double value) {
    if(!m_enabled || index<0 || size_t(index)>=m_inputs.size()) {return std::nullopt;}
    auto &in=m_inputs[size_t(index)];
    if(!in.latched || !in.shown || in.writes>0) {return std::nullopt;}
    in.ramping=false;
    setValue(in,clampValue(in,value));
    return in.value;
}
void CommandModel::remove(int index) {
    if(index<0 || size_t(index)>=m_inputs.size()) {return;}
    auto &in=m_inputs[size_t(index)];
    in.shown=false;in.latched=false;in.held=false;in.ramping=false;
    setValue(in,0);
}
void CommandModel::holdAll() {
    for(auto &in:m_inputs) {in.latched=false;in.held=false;in.ramping=false;setValue(in,0);}
}
void CommandModel::beginWrite(uint8_t type,uint8_t instance) {
    for(auto &in:m_inputs) {
        if(in.ref.type!=type || in.ref.instance!=instance) {continue;}
        ++in.writes;in.latched=false;in.held=false;in.ramping=false;setValue(in,0);
    }
}
void CommandModel::endWrite(uint8_t type,uint8_t instance) {
    for(auto &in:m_inputs) {
        if(in.ref.type==type && in.ref.instance==instance && in.writes>0) {--in.writes;}
    }
    zeroInstance(type,instance);
}
void CommandModel::zeroInstance(uint8_t type,uint8_t instance) {
    for(auto &in:m_inputs) {
        if(in.ref.type!=type || in.ref.instance!=instance) {continue;}
        in.latched=false;in.held=false;in.ramping=false;setValue(in,0);
        // A fresh frame even at 0: a mode change waits for one.
        if(m_enabled) {m_groups[in.group].dirty=true;}
    }
}
std::vector<CommandModel::Frame> CommandModel::tick(int64_t now) {
    std::vector<Frame> frames;
    if(!m_enabled) {return frames;}
    for(auto &in:m_inputs) {
        if(!in.ramping) {continue;}
        const int64_t k=(now-in.rampStart)/kStepMs;
        if(k>=kRampSteps) {in.ramping=false;setValue(in,0);continue;}
        if(k<=0) {continue;}
        // Toward zero on the shown step: the ramp never grows the value.
        const double target=in.rampFrom*double(kRampSteps-k)/kRampSteps;
        setValue(in,roundTo(std::trunc(target/in.step)*in.step,in.decimals));
    }
    for(auto &g:m_groups) {
        if(!g.dirty || (g.sentOnce && now-g.lastSent<kStepMs)) {continue;}
        Frame frame;frame.group=g.id;
        for(const auto i:g.inputs) {frame.bits.push_back(numberToBits(m_inputs[i].descriptor.value_type,m_inputs[i].value));}
        g.dirty=false;g.lastSent=now;g.sentOnce=true;
        frames.push_back(std::move(frame));
    }
    return frames;
}
void CommandModel::retry(uint8_t group) {
    if(!m_enabled) {return;}
    for(auto &g:m_groups) {if(g.id==group) {g.dirty=true;}}
}
bool CommandModel::active() const {
    if(!m_enabled) {return false;}
    for(const auto &in:m_inputs) {if(in.ramping) {return true;}}
    for(const auto &g:m_groups) {if(g.dirty) {return true;}}
    return false;
}
} // namespace drivelink::gui
