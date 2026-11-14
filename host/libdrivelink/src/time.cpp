#include "drivelink/time.hpp"
#include "drivelink/session.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace drivelink {
uint64_t monotonicUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
double ClockModel::map(uint64_t device_us) const {
    if (!offset_valid) { return std::numeric_limits<double>::quiet_NaN(); }
    const double delta=device_us>=device_reference_us ? static_cast<double>(device_us-device_reference_us)
        : -static_cast<double>(device_reference_us-device_us);
    return static_cast<double>(host_reference_us)+slope*delta;
}
ClockEstimator::ClockEstimator(ClockOptions options):m_options(options) {
    if (!options.bin_us || options.window_us<options.bin_us || options.window_us/options.bin_us>4096 ||
        options.min_samples<2 || options.capacity<options.min_samples || options.capacity>65536 ||
        !options.min_span_us || !options.stale_us) { throw Error(ErrorKind::Usage,"invalid clock options"); }
    m_points.reserve(options.capacity);
    m_selected.resize(static_cast<size_t>(options.window_us/options.bin_us)+2);
}
void ClockEstimator::reset() { m_points.clear();m_generation=0;m_last_device=0;m_last_host=0; }
bool ClockEstimator::measure(const TimeSample &s,double &rtt,double &device,double &host) {
    constexpr auto max_clock=static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (s.host_rx_us>max_clock || s.esp_tx_us>max_clock ||
        s.host_rx_us<s.host_tx_us || s.esp_tx_us<s.esp_rx_us) { return false; }
    const auto elapsed=s.host_rx_us-s.host_tx_us, processing=s.esp_tx_us-s.esp_rx_us;
    if (elapsed<processing) { return false; }
    rtt=static_cast<double>(elapsed-processing);
    device=static_cast<double>(s.esp_rx_us)+static_cast<double>(processing)*0.5;
    host=static_cast<double>(s.host_tx_us)+static_cast<double>(elapsed)*0.5;
    return std::isfinite(device) && std::isfinite(host) && std::isfinite(rtt);
}
bool ClockEstimator::add(const TimeSample &s) {
    double rtt=0,device=0,host=0;
    if (!measure(s,rtt,device,host)) { return false; }
    if (!m_points.empty() && (s.boot_generation!=m_generation || s.esp_rx_us<m_last_device)) { reset(); }
    if (!m_points.empty() && s.host_rx_us<=m_last_host) { return false; }
    m_generation=s.boot_generation;m_last_device=s.esp_rx_us;m_last_host=s.host_rx_us;
    const auto oldest=s.host_rx_us>m_options.window_us?s.host_rx_us-m_options.window_us:0;
    m_points.erase(m_points.begin(),std::find_if(m_points.begin(),m_points.end(),
        [oldest](const Point &p){return p.received>=oldest;}));
    if (m_points.size()==m_options.capacity) { m_points.erase(m_points.begin()); }
    m_points.push_back({s.host_rx_us,device,host,rtt});return true;
}
ClockModel ClockEstimator::model(uint64_t now) const {
    ClockModel result;
    if (m_points.empty()) { return result; }
    result.updated_us=m_last_host;result.age_us=now>=m_last_host?now-m_last_host:0;
    if(now<m_last_host || result.age_us>m_options.stale_us) { return result; }
    // Keep the lowest RTT in each absolute host-time bin. Fixed local traversal;
    // scratch storage belongs to this estimator, with no allocation per fit.
    auto &selected=m_selected;
    std::fill(selected.begin(),selected.end(),m_points.size());
    const auto oldest=now>m_options.window_us?now-m_options.window_us:0;
    const auto first_bin=oldest/m_options.bin_us;
    for (size_t i=0;i<m_points.size();++i) {
        const auto &p=m_points[i];if(p.received<oldest) { continue; }
        const auto bin=static_cast<size_t>(p.received/m_options.bin_us-first_bin);
        if (bin>=selected.size()) { continue; }
        if (selected[bin]==m_points.size() || p.rtt<m_points[selected[bin]].rtt) { selected[bin]=i; }
    }
    double x=0,y=0;size_t first=m_points.size(),last=0;
    result.min_rtt_us=std::numeric_limits<double>::infinity();
    for(const auto i:selected) { if(i==m_points.size()) { continue; }
        const auto &p=m_points[i];x+=p.device;y+=p.host;++result.samples;
        first=std::min(first,i);last=std::max(last,i);
        result.min_rtt_us=std::min(result.min_rtt_us,p.rtt);result.max_rtt_us=std::max(result.max_rtt_us,p.rtt);
    }
    if(!result.samples) { return {}; }
    x/=static_cast<double>(result.samples);y/=static_cast<double>(result.samples);
    result.span_us=m_points[last].received-m_points[first].received;
    double xx=0,xy=0;
    for(const auto i:selected) {if(i==m_points.size()){continue;} const auto &p=m_points[i];
        xx+=(p.device-x)*(p.device-x);xy+=(p.device-x)*(p.host-y);}
    result.drift_valid=result.samples>=m_options.min_samples && result.span_us>=m_options.min_span_us && xx>0;
    result.slope=result.drift_valid?xy/xx:1;
    if(!std::isfinite(result.slope) || result.slope<=0) { return {}; }
    result.offset_valid=true;result.device_reference_us=static_cast<uint64_t>(x);
    const auto reference=y+result.slope*(static_cast<double>(result.device_reference_us)-x);
    if(!std::isfinite(reference) || reference<0 || reference>static_cast<double>(std::numeric_limits<int64_t>::max())) { return {}; }
    result.host_reference_us=static_cast<uint64_t>(reference);
    result.updated_us=m_last_host;result.drift_ppm=(result.slope-1)*1000000;
    double residual=0;
    for(const auto i:selected) {if(i==m_points.size()){continue;}const auto &p=m_points[i];
        const auto error=p.host-(y+result.slope*(p.device-x));residual+=error*error;}
    result.residual_us=std::sqrt(residual/static_cast<double>(result.samples));return result;
}
}
