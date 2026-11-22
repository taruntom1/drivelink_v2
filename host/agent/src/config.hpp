#pragma once
// drivelink-agent configuration: one JSON file plus command-line flags that
// override it (host/agent/README.md). Pure: no I/O except reading the files
// named by loadConfig/--token-file. Errors throw ConfigError with a message
// that names the key or flag.
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace drivelink::agent
{
struct ConfigError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct DeviceConfig
{
    std::string alias;     // 1..32 of [a-z0-9_-], starts with a letter, never 12 hex digits
    std::string endpoint;  // serial://<path> or tcp://<numeric-ip>:<port>
    unsigned baud = 921600; // serial only; 9600..4000000
};

struct AgentConfig
{
    std::string bind = "127.0.0.1"; // numeric IPv4/IPv6 address
    uint16_t port = 47620;          // 0 = ephemeral (tests)
    std::string token;              // required: 8..128 bytes, printable ASCII 0x21..0x7E
    std::chrono::milliseconds hello_timeout{5000}; // 100..60000
    std::chrono::milliseconds retry{2000};         // 100..60000
    std::chrono::milliseconds request_timeout{3000}; // 100..60000, device request timeout
    size_t history_size = 256;                       // 16..4096 entries per device
    bool advertise = true;                           // mDNS advertising (only when bind is not loopback)
    std::string name;                                // mDNS instance name; empty = the host name
    std::vector<DeviceConfig> devices;              // 0..8, unique aliases and endpoints
    bool loopback() const;                           // bind is 127.0.0.0/8 or ::1
};

// Parse the JSON text of a config file. Unknown keys are errors. Keys:
// bind, port, token, token_file (relative to base_dir), hello_timeout_ms,
// retry_ms, request_timeout_ms, history_size, advertise (boolean), name (mDNS
// instance, 1..63 bytes, see discovery::instanceError), devices: [{alias,
// endpoint, baud}]. token and token_file are mutually exclusive.
AgentConfig parseConfig(std::string_view json_text, const std::string &base_dir = ".");

struct CommandLine
{
    std::optional<std::string> config_path; // --config PATH
    bool help = false, version = false;     // --help, --version
    bool list_ports = false;                // --list-ports
};
// The --config value, if any (main reads that file with loadConfig first).
std::optional<std::string> findConfigPath(std::span<const std::string_view> args);
// Read and parse a config file; relative token_file paths resolve against its directory.
AgentConfig loadConfig(const std::string &path);
// Apply flags over `config` (which may come from parseConfig or be default).
// Flags: --config PATH (recorded only), --bind ADDR, --port N, --token TOKEN,
// --token-file PATH, --device ALIAS=ENDPOINT[@BAUD] (repeatable; any --device
// replaces the file's device list), --hello-timeout-ms N, --retry-ms N,
// --request-timeout-ms N, --history-size N, --name NAME, --no-advertise (no
// value), --list-ports, --help, --version.
CommandLine parseCommandLine(std::span<const std::string_view> args, AgentConfig &config);
// Final checks after flags (token present, at most 8 devices, uniqueness).
void validate(const AgentConfig &config);
// Usage text for --help.
std::string usage();
} // namespace drivelink::agent
