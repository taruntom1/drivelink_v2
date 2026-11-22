#pragma once
// drivelink-agent server (docs/protocol/agent.md). One Asio thread owns every
// client socket, the routing state and each device's group map; each device
// has its own libdrivelink Session (own I/O thread) whose completions and
// frames are dispatched onto that thread.
#include "config.hpp"
#include <cstdint>
#include <memory>

namespace drivelink::agent
{
class Agent
{
  public:
    explicit Agent(AgentConfig config);
    ~Agent();
    Agent(const Agent &) = delete;
    Agent &operator=(const Agent &) = delete;
    // Bind and listen; returns the bound port. Throws std::runtime_error.
    uint16_t start();
    // Serve until stop() or SIGINT/SIGTERM. Devices open in the background.
    void run();
    // Thread-safe: Closing(Shutdown) to every client, DISARM to every online
    // device (best effort), then run() returns.
    void stop();

    struct Impl; // agent.cpp

  private:
    std::unique_ptr<Impl> m;
};
} // namespace drivelink::agent
