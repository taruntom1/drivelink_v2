// drivelink-agent: owns device ports and shares them over TCP (host/agent/README.md).
#include "agent.hpp"
#include "config.hpp"
#include "ports.hpp"
#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    using namespace drivelink::agent;
    try
    {
        std::vector<std::string_view> args(argv + 1, argv + argc);
        AgentConfig config;
        if (auto path = findConfigPath(args)) { config = loadConfig(*path); }
        const auto line = parseCommandLine(args, config);
        if (line.help) { std::cout << usage(); return 0; }
        if (line.version) { std::cout << "drivelink-agent " << DRIVELINK_AGENT_VERSION << "\n"; return 0; }
        if (line.list_ports)
        {
            const auto ports = findPorts();
            if (ports.empty())
            {
                std::cerr << "drivelink-agent: no ESP port found (/dev/drivelink/*, /dev/serial0, /dev/ttyAMA*)\n";
                return 1;
            }
            std::cout << portListing(ports);
            return 0;
        }
        validate(config);
        const auto bind = config.bind.find(':') == std::string::npos ? config.bind : "[" + config.bind + "]";
        Agent agent(std::move(config));
        const auto port = agent.start();
        // Scripts and tests wait for this line; devices may still be connecting.
        std::cout << "READY tcp://" << bind << ":" << port << std::endl;
        agent.run();
        return 0;
    }
    catch (const ConfigError &e)
    {
        std::cerr << "drivelink-agent: " << e.what() << "\n(see drivelink-agent --help)\n";
        return 2;
    }
    catch (const std::exception &e)
    {
        std::cerr << "drivelink-agent: " << e.what() << "\n";
        return 1;
    }
}
