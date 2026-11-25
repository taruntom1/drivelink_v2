#include "doctest.h"

#include <string>

#include "drivelink/protocol/defs.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/link.hpp"

namespace p = drivelink::protocol;
namespace sys = drivelink::protocol::defs::system;

TEST_CASE("Link rates are the link.baud enum, in order")
{
    const p::defs::ItemDesc *item = p::defs::findItem(sys::kDesc, sys::kLinkBaud);
    REQUIRE(item != nullptr);
    REQUIRE(item->enum_values.size() == p::kLinkBaudRates.size());
    for (size_t i = 0; i < p::kLinkBaudRates.size(); ++i) {
        CHECK(item->enum_values[i].value == i);
        CHECK(std::string(item->enum_values[i].name) == "baud_" + std::to_string(p::kLinkBaudRates[i]));
        CHECK(p::linkBaudRate(static_cast<uint32_t>(i)) == p::kLinkBaudRates[i]);
        CHECK(p::linkBaudValue(p::kLinkBaudRates[i]) == i);
    }
    CHECK(p::linkBaudRate(item->default_bits) == 921600);
    CHECK(p::linkBaudRate(8) == 0);
    CHECK(p::linkBaudRate(255) == 0);
    CHECK_FALSE(p::linkBaudValue(9600).has_value());
    CHECK_FALSE(p::linkBaudValue(0).has_value());
}
