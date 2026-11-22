#include "doctest.h"
#include "drivelink/session.hpp"
#include <cmath>
using namespace drivelink;
TEST_CASE("mapped timestamps expose generation and growing uncertainty") {
    ClockModel model;CHECK(std::isinf(model.uncertainty(10)));
    auto missing=mapTime(model,10,7);CHECK_FALSE(missing.valid);CHECK(missing.generation==7);CHECK(std::isnan(missing.host_us));
    model.offset_valid=true;model.device_reference_us=1000;model.host_reference_us=5000;
    model.slope=1.001;model.min_rtt_us=100;model.residual_us=2;model.span_us=1000000;model.age_us=20000;
    const auto mapped=mapTime(model,2000,9);CHECK(mapped.valid);CHECK(mapped.device_us==2000);CHECK(mapped.generation==9);
    CHECK(mapped.host_us==doctest::Approx(6001));
    CHECK(model.uncertainty(1000)==doctest::Approx(54));CHECK(model.uncertainty(1001000)>model.uncertainty(1000));
}
namespace {
TimeSample sample(uint64_t t,double slope=1,double offset=3000000,double delay=100) {
    const auto host=static_cast<uint64_t>(static_cast<double>(t)*slope+offset);
    return {0,host-static_cast<uint64_t>(delay),t,t,host+static_cast<uint64_t>(delay),0};
}
}
TEST_CASE("clock fit handles both drift signs, large origins and low RTT selection") {
    for(const double slope:{0.9999,1.0001}) {
        ClockEstimator fit;
        for(uint64_t i=0;i<100;++i) {
            auto s=sample(1000000000000ULL+i*1000000,slope);
            REQUIRE(fit.add(s));
            // A delayed asymmetric observation in the same bin must lose.
            auto noisy=sample(1000000000000ULL+i*1000000+100,slope,3005000,50000);
            REQUIRE(fit.add(noisy));
        }
        const auto m=fit.model(sample(1000000000000ULL+99000100,slope,3005000,50000).host_rx_us);
        REQUIRE(m.drift_valid);CHECK(m.slope==doctest::Approx(slope).epsilon(0.00000001));
        CHECK(m.map(1000000050000ULL)==doctest::Approx(1000000050000.0*slope+3000000).epsilon(1e-11));
        CHECK(m.min_rtt_us==200);CHECK(m.max_rtt_us==200);CHECK(m.residual_us<1);
    }
}
TEST_CASE("clock validity, aging, discontinuities and invalid sample rejection") {
    ClockEstimator fit;CHECK_FALSE(fit.model(0).offset_valid);CHECK(std::isnan(fit.model(0).map(5)));
    auto s=sample(1000000);REQUIRE(fit.add(s));
    CHECK(fit.model(s.host_rx_us).offset_valid);CHECK_FALSE(fit.model(s.host_rx_us).drift_valid);
    CHECK_FALSE(fit.add(s));
    auto bad=s;bad.host_rx_us=bad.host_tx_us-1;CHECK_FALSE(fit.add(bad));
    bad=s;bad.esp_tx_us=bad.esp_rx_us-1;CHECK_FALSE(fit.add(bad));
    bad=s;bad.esp_tx_us+=1000;CHECK_FALSE(fit.add(bad));
    CHECK_FALSE(fit.model(s.host_rx_us+5000001).offset_valid);
    for(uint64_t i=2;i<50;++i){REQUIRE(fit.add(sample(i*1000000)));}
    CHECK(fit.model(sample(49000000).host_rx_us).drift_valid);
    s=sample(50000000);s.boot_generation=1;REQUIRE(fit.add(s));CHECK(fit.model(s.host_rx_us).samples==1);
    s=sample(1,1,100000000);s.boot_generation=1;REQUIRE(fit.add(s));CHECK(fit.model(s.host_rx_us).samples==1);
    fit.reset();CHECK_FALSE(fit.model(s.host_rx_us).offset_valid);
    ClockOptions o;o.capacity=1;CHECK_THROWS_AS(ClockEstimator{o},Error);
}
TEST_CASE("clock sliding window and finite capacity evict old anchors") {
    ClockOptions o;o.capacity=12;ClockEstimator fit(o);
    for(uint64_t i=0;i<300;++i){REQUIRE(fit.add(sample(100000000+i*1000000)));}
    const auto m=fit.model(sample(399000000).host_rx_us);
    CHECK(m.samples<=4);CHECK(m.span_us<=11000000);CHECK_FALSE(m.drift_valid);
    // A constant asymmetric delay biases offset; RTT filtering cannot remove it.
    ClockEstimator asymmetric;
    for(uint64_t i=0;i<50;++i){auto s=sample(10000000+i*1000000);s.host_rx_us+=1000;REQUIRE(asymmetric.add(s));}
    const auto model=asymmetric.model(sample(59000000).host_rx_us+1000);
    CHECK(model.map(59000000)==doctest::Approx(62000500).epsilon(1e-7));
}
