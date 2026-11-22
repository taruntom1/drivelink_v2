#include "../src/recovery.hpp"

#include <gtest/gtest.h>

using drivelink_bringup::controllersToActivate;
using drivelink_bringup::activeSwitchTargets;

namespace
{
struct Controller
{
    std::string name, state;
};

TEST(Recovery, OnlyConfiguredInactiveControllersAreStarted)
{
    const std::vector<Controller> controllers{{"drive", "inactive"}, {"imu", "active"},
                                              {"joint_states", "inactive"}, {"other", "inactive"}};
    EXPECT_EQ(controllersToActivate({"joint_states", "drive", "imu"}, controllers),
              (std::vector<std::string>{"joint_states", "drive"}));
    EXPECT_TRUE(controllersToActivate({"imu"}, controllers).empty());
}

TEST(Recovery, MissingOrUnconfiguredControllerRefusesTheWholeRequest)
{
    const std::vector<Controller> controllers{{"drive", "inactive"}, {"imu", "unconfigured"}};
    EXPECT_THROW(controllersToActivate({"drive", "missing"}, controllers), std::runtime_error);
    EXPECT_THROW(controllersToActivate({"drive", "imu"}, controllers), std::runtime_error);
}

TEST(Recovery, OnlyActiveControllersFromTheAttemptAreStopped)
{
    const std::vector<Controller> controllers{{"drive", "active"}, {"imu", "inactive"}, {"joints", "active"}};
    EXPECT_EQ(activeSwitchTargets({"drive", "imu"}, controllers), std::vector<std::string>{"drive"});
}
} // namespace
