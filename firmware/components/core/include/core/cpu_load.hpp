#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
namespace core {
// Runtime counters and timestamps share a microsecond clock. Unsigned
// subtraction handles 32-bit wrap; delayed idle accounting is clamped.
class CpuLoadSampler {
public:
    std::array<uint8_t,2> sample(uint32_t now,std::array<uint32_t,2> idle) {
        std::array<uint8_t,2> load{255,255};
        const uint32_t elapsed=now-m_previous_time;
        if(m_initialized && elapsed) {
            for(std::size_t i=0;i<2;++i) {
                const uint64_t percentage=static_cast<uint64_t>(idle[i]-m_previous_idle[i])*100/elapsed;
                load[i]=static_cast<uint8_t>(100-std::min<uint64_t>(100,percentage));
            }
        }
        m_previous_time=now;m_previous_idle=idle;m_initialized=true;return load;
    }
private:
    bool m_initialized=false;
    uint32_t m_previous_time=0;
    std::array<uint32_t,2> m_previous_idle{};
};
}
