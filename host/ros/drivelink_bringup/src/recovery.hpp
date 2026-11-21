#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace drivelink_bringup
{
// The controller manager leaves affected controllers inactive when hardware
// errors. An unaffected controller may already be active; leave it alone.
template<class Controllers>
std::vector<std::string> controllersToActivate(const std::vector<std::string> &wanted,
                                               const Controllers &controllers)
{
    std::vector<std::string> pending;
    for (const auto &name : wanted)
    {
        const auto found = std::ranges::find_if(controllers, [&](const auto &c) { return c.name == name; });
        if (found == controllers.end()) { throw std::runtime_error("controller is not loaded: " + name); }
        if (found->state == "active") { continue; }
        if (found->state != "inactive")
        {
            throw std::runtime_error("controller " + name + " is " + found->state + "; configure it first");
        }
        pending.push_back(name);
    }
    return pending;
}

template<class Controllers>
std::vector<std::string> activeSwitchTargets(const std::vector<std::string> &attempted,
                                             const Controllers &controllers)
{
    std::vector<std::string> active;
    for (const auto &controller : controllers)
    {
        if (controller.state == "active" && std::ranges::find(attempted, controller.name) != attempted.end())
        {
            active.push_back(controller.name);
        }
    }
    return active;
}
} // namespace drivelink_bringup
