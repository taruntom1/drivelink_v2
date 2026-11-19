// drivelink-agent configuration (Linux only: uses inet_pton for address checks).
#include "config.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

#include "drivelink/discovery.hpp"

namespace drivelink::agent
{
namespace
{
using json = nlohmann::json;

constexpr int64_t kMinMs = 100, kMaxMs = 60000;
constexpr int64_t kMinHistory = 16, kMaxHistory = 4096;
constexpr std::size_t kMaxDevices = 8;

std::string single_quoted(std::string_view s)
{
    return "'" + std::string(s) + "'";
}

bool parseUint(std::string_view s, uint64_t &out)
{
    if (s.empty())
        return false;
    const char *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

// Numeric text for a flag value, range-checked.
int64_t flagInt(std::string_view flag, std::string_view text, int64_t lo, int64_t hi)
{
    uint64_t n = 0;
    if (!parseUint(text, n))
        throw ConfigError(std::string(flag) + " needs a whole number, got " + single_quoted(text));
    if (n < static_cast<uint64_t>(lo) || n > static_cast<uint64_t>(hi))
        throw ConfigError(std::string(flag) + " must be " + std::to_string(lo) + ".." +
                          std::to_string(hi) + ", got " + single_quoted(text));
    return static_cast<int64_t>(n);
}

int64_t jsonInt(const json &v, std::string_view what, int64_t lo, int64_t hi)
{
    if (!v.is_number_integer())
        throw ConfigError(std::string(what) + " must be an integer");
    if (v.is_number_unsigned() && v.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw ConfigError(std::string(what) + " must be " + std::to_string(lo) + ".." + std::to_string(hi));
    const int64_t n = v.get<int64_t>();
    if (n < lo || n > hi)
        throw ConfigError(std::string(what) + " must be " + std::to_string(lo) + ".." + std::to_string(hi));
    return n;
}

std::string jsonString(const json &v, std::string_view what)
{
    if (!v.is_string())
        throw ConfigError(std::string(what) + " must be a string");
    return v.get<std::string>();
}

// An mDNS instance name that is not empty must satisfy the library's rules.
void checkName(std::string_view name, std::string_view what)
{
    const std::string why = drivelink::discovery::instanceError(name);
    if (!why.empty())
        throw ConfigError(std::string(what) + ": " + why);
}

void checkBind(std::string_view bind, std::string_view what)
{
    const std::string text(bind);
    in6_addr a6{};
    in_addr a4{};
    if (inet_pton(AF_INET, text.c_str(), &a4) != 1 && inet_pton(AF_INET6, text.c_str(), &a6) != 1)
        throw ConfigError(std::string(what) + " must be a numeric IPv4 or IPv6 address, got " + single_quoted(bind));
}

void checkToken(std::string_view token, std::string_view what)
{
    if (token.size() < 8 || token.size() > 128)
        throw ConfigError(std::string(what) + " must be 8..128 bytes");
    for (unsigned char c : token)
        if (c < 0x21 || c > 0x7E)
            throw ConfigError(std::string(what) + " must be printable ASCII without spaces");
}

// First line of the file, trailing CR/LF/spaces removed. Reads at most 1 KiB
// (a longer first line is not a valid token anyway).
std::string readTokenFile(const std::filesystem::path &path, std::string_view what)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ConfigError(std::string(what) + ": cannot read " + single_quoted(path.string()));
    std::array<char, 1024> buf{};
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::string line(buf.data(), static_cast<std::size_t>(in.gcount()));
    if (const auto nl = line.find('\n'); nl != std::string::npos)
        line.resize(nl);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    checkToken(line, std::string(what) + " content");
    return line;
}

void checkAlias(std::string_view alias, std::string_view what)
{
    const auto bad = [&](const char *why) {
        throw ConfigError(std::string(what) + " " + single_quoted(alias) + ": " + why);
    };
    if (alias.empty() || alias.size() > 32)
        bad("alias must be 1..32 characters");
    if (alias[0] < 'a' || alias[0] > 'z')
        bad("alias must start with a lowercase letter");
    for (char c : alias)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            bad("alias may contain only a-z, 0-9, '_' and '-'");
    if (alias.size() == 12 &&
        std::all_of(alias.begin(), alias.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        bad("alias must not look like a device id (12 hex digits)");
}

constexpr std::string_view kSerial = "serial://", kTcp = "tcp://";

bool isTcp(std::string_view endpoint)
{
    return endpoint.starts_with(kTcp);
}

void checkEndpoint(std::string_view endpoint, std::string_view what)
{
    if (endpoint.starts_with(kSerial))
    {
        if (endpoint.size() == kSerial.size())
            throw ConfigError(std::string(what) + ": serial endpoint needs a path (serial:///dev/ttyACM0)");
        return;
    }
    if (isTcp(endpoint))
    {
        const std::string_view rest = endpoint.substr(kTcp.size());
        const auto colon = rest.rfind(':');
        if (colon == std::string_view::npos)
            throw ConfigError(std::string(what) + ": tcp endpoint must be tcp://<IPv4>:<port>");
        const std::string host(rest.substr(0, colon));
        in_addr a4{};
        if (inet_pton(AF_INET, host.c_str(), &a4) != 1)
            throw ConfigError(std::string(what) + ": tcp endpoint host must be a numeric IPv4 address");
        uint64_t port = 0;
        if (!parseUint(rest.substr(colon + 1), port) || port < 1 || port > 65535)
            throw ConfigError(std::string(what) + ": tcp endpoint port must be 1..65535");
        return;
    }
    throw ConfigError(std::string(what) + ": endpoint must start with serial:// or tcp://, got " + single_quoted(endpoint));
}

DeviceConfig makeDevice(std::string_view alias, std::string_view endpoint, std::optional<int64_t> baud,
                        std::string_view what)
{
    checkAlias(alias, what);
    const std::string where = std::string(what) + " " + single_quoted(alias);
    checkEndpoint(endpoint, where);
    if (baud && isTcp(endpoint))
        throw ConfigError(where + ": baud applies only to serial endpoints");
    DeviceConfig d;
    d.alias = std::string(alias);
    d.endpoint = std::string(endpoint);
    if (baud)
        d.baud = static_cast<unsigned>(*baud);
    return d;
}

DeviceConfig parseDeviceJson(const json &j, std::size_t index)
{
    const std::string where = "devices[" + std::to_string(index) + "]";
    if (!j.is_object())
        throw ConfigError(where + " must be an object");
    std::optional<std::string> alias, endpoint;
    std::optional<int64_t> baud;
    for (const auto &[key, value] : j.items())
    {
        if (key == "alias")
            alias = jsonString(value, where + ".alias");
        else if (key == "endpoint")
            endpoint = jsonString(value, where + ".endpoint");
        else if (key == "baud")
            baud = jsonInt(value, where + ".baud", 9600, 4000000);
        else
            throw ConfigError("unknown key " + single_quoted(key) + " in " + where);
    }
    if (!alias)
        throw ConfigError(where + " is missing 'alias'");
    if (!endpoint)
        throw ConfigError(where + " is missing 'endpoint'");
    return makeDevice(*alias, *endpoint, baud, "device");
}

DeviceConfig parseDeviceFlag(std::string_view spec)
{
    const auto eq = spec.find('=');
    if (eq == std::string_view::npos)
        throw ConfigError("--device must be ALIAS=ENDPOINT[@BAUD], got " + single_quoted(spec));
    const std::string_view alias = spec.substr(0, eq);
    std::string_view endpoint = spec.substr(eq + 1);
    std::optional<int64_t> baud;
    if (const auto at = endpoint.rfind('@'); at != std::string_view::npos)
    {
        baud = flagInt("--device baud", endpoint.substr(at + 1), 9600, 4000000);
        endpoint = endpoint.substr(0, at);
    }
    return makeDevice(alias, endpoint, baud, "--device");
}

} // namespace

bool AgentConfig::loopback() const
{
    in_addr a4{};
    if (inet_pton(AF_INET, bind.c_str(), &a4) == 1)
        return (ntohl(a4.s_addr) >> 24) == 127;
    in6_addr a6{};
    if (inet_pton(AF_INET6, bind.c_str(), &a6) == 1)
        return std::memcmp(&a6, &in6addr_loopback, sizeof a6) == 0;
    return false;
}

AgentConfig parseConfig(std::string_view json_text, const std::string &base_dir)
{
    json root;
    try
    {
        root = json::parse(json_text.begin(), json_text.end());
    }
    catch (const json::exception &e)
    {
        throw ConfigError(std::string("config is not valid JSON: ") + e.what());
    }
    if (!root.is_object())
        throw ConfigError("config must be a JSON object");

    AgentConfig c;
    std::optional<std::string> token_file;
    bool have_token = false;
    for (const auto &[key, value] : root.items())
    {
        if (key == "bind")
        {
            c.bind = jsonString(value, "bind");
            checkBind(c.bind, "bind");
        }
        else if (key == "port")
            c.port = static_cast<uint16_t>(jsonInt(value, "port", 0, 65535));
        else if (key == "token")
        {
            c.token = jsonString(value, "token");
            checkToken(c.token, "token");
            have_token = true;
        }
        else if (key == "token_file")
            token_file = jsonString(value, "token_file");
        else if (key == "hello_timeout_ms")
            c.hello_timeout = std::chrono::milliseconds(jsonInt(value, key, kMinMs, kMaxMs));
        else if (key == "retry_ms")
            c.retry = std::chrono::milliseconds(jsonInt(value, key, kMinMs, kMaxMs));
        else if (key == "request_timeout_ms")
            c.request_timeout = std::chrono::milliseconds(jsonInt(value, key, kMinMs, kMaxMs));
        else if (key == "history_size")
            c.history_size = static_cast<size_t>(jsonInt(value, key, kMinHistory, kMaxHistory));
        else if (key == "advertise")
        {
            if (!value.is_boolean())
                throw ConfigError("advertise must be true or false");
            c.advertise = value.get<bool>();
        }
        else if (key == "name")
        {
            c.name = jsonString(value, "name");
            checkName(c.name, "name");
        }
        else if (key == "devices")
        {
            if (!value.is_array())
                throw ConfigError("devices must be an array");
            c.devices.clear();
            std::size_t i = 0;
            for (const auto &d : value)
                c.devices.push_back(parseDeviceJson(d, i++));
        }
        else
            throw ConfigError("unknown config key " + single_quoted(key));
    }
    if (token_file)
    {
        if (have_token)
            throw ConfigError("token and token_file are mutually exclusive");
        if (token_file->empty())
            throw ConfigError("token_file must not be empty");
        c.token = readTokenFile(std::filesystem::path(base_dir) / *token_file, "token_file");
    }
    return c;
}

std::optional<std::string> findConfigPath(std::span<const std::string_view> args)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view a = args[i];
        if (a == "--config")
        {
            // A missing value is reported by parseCommandLine.
            if (i + 1 >= args.size())
                return std::nullopt;
            return std::string(args[i + 1]);
        }
        if (a.starts_with("--config="))
            return std::string(a.substr(9));
    }
    return std::nullopt;
}

AgentConfig loadConfig(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ConfigError("cannot read config file " + single_quoted(path));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        throw ConfigError("cannot read config file " + single_quoted(path));
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty())
        dir = ".";
    return parseConfig(text, dir.string());
}

CommandLine parseCommandLine(std::span<const std::string_view> args, AgentConfig &config)
{
    static constexpr std::array<std::string_view, 11> kValueFlags = {
        "--config", "--bind", "--port", "--token", "--token-file",
        "--device", "--hello-timeout-ms", "--retry-ms", "--request-timeout-ms", "--history-size", "--name"};

    CommandLine cl;
    bool devices_replaced = false, token_flag = false, token_file_flag = false;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view arg = args[i];
        if (!arg.starts_with("--"))
            throw ConfigError("unexpected argument " + single_quoted(arg));
        std::string_view name = arg, inline_value;
        bool has_inline = false;
        if (const auto eq = arg.find('='); eq != std::string_view::npos)
        {
            name = arg.substr(0, eq);
            inline_value = arg.substr(eq + 1);
            has_inline = true;
        }
        if (name == "--help" || name == "--version")
        {
            if (has_inline)
                throw ConfigError(std::string(name) + " takes no value");
            (name == "--help" ? cl.help : cl.version) = true;
            continue;
        }
        if (name == "--no-advertise")
        {
            if (has_inline)
                throw ConfigError("--no-advertise takes no value");
            config.advertise = false;
            continue;
        }
        if (std::find(kValueFlags.begin(), kValueFlags.end(), name) == kValueFlags.end())
            throw ConfigError("unknown flag " + single_quoted(name));
        std::string_view value = inline_value;
        if (!has_inline)
        {
            if (i + 1 >= args.size())
                throw ConfigError(std::string(name) + " needs a value");
            value = args[++i];
        }

        if (name == "--config")
        {
            if (value.empty())
                throw ConfigError("--config needs a path");
            cl.config_path = std::string(value);
        }
        else if (name == "--bind")
        {
            checkBind(value, "--bind");
            config.bind = std::string(value);
        }
        else if (name == "--port")
            config.port = static_cast<uint16_t>(flagInt(name, value, 0, 65535));
        else if (name == "--token")
        {
            if (token_file_flag)
                throw ConfigError("--token and --token-file are mutually exclusive");
            checkToken(value, "--token");
            config.token = std::string(value);
            token_flag = true;
        }
        else if (name == "--token-file")
        {
            if (token_flag)
                throw ConfigError("--token and --token-file are mutually exclusive");
            if (value.empty())
                throw ConfigError("--token-file needs a path");
            config.token = readTokenFile(std::filesystem::path(std::string(value)), "--token-file");
            token_file_flag = true;
        }
        else if (name == "--name")
        {
            checkName(value, "--name");
            config.name = std::string(value);
        }
        else if (name == "--device")
        {
            if (!devices_replaced)
            {
                config.devices.clear();
                devices_replaced = true;
            }
            config.devices.push_back(parseDeviceFlag(value));
        }
        else if (name == "--hello-timeout-ms")
            config.hello_timeout = std::chrono::milliseconds(flagInt(name, value, kMinMs, kMaxMs));
        else if (name == "--retry-ms")
            config.retry = std::chrono::milliseconds(flagInt(name, value, kMinMs, kMaxMs));
        else if (name == "--history-size")
            config.history_size = static_cast<size_t>(flagInt(name, value, kMinHistory, kMaxHistory));
        else // --request-timeout-ms
            config.request_timeout = std::chrono::milliseconds(flagInt(name, value, kMinMs, kMaxMs));
    }
    return cl;
}

void validate(const AgentConfig &config)
{
    if (config.token.empty())
        throw ConfigError("token is required (token, token_file, --token or --token-file)");
    checkToken(config.token, "token");
    if (config.devices.empty())
        throw ConfigError("at least one device is required (devices or --device)");
    if (config.devices.size() > kMaxDevices)
        throw ConfigError("at most " + std::to_string(kMaxDevices) + " devices are allowed");
    std::set<std::string> aliases, endpoints;
    for (const auto &d : config.devices)
    {
        if (!aliases.insert(d.alias).second)
            throw ConfigError("duplicate device alias " + single_quoted(d.alias));
        if (!endpoints.insert(d.endpoint).second)
            throw ConfigError("duplicate device endpoint " + single_quoted(d.endpoint));
    }
    // The ListDevices answer is one frame: agent response header (5), count,
    // then per device alias, online, id, a name of up to 16 bytes, endpoint,
    // clients, the holder's id, a client name of up to 32 bytes and its kind
    // (docs/protocol/agent.md). It must fit MAX_PAYLOAD (1024).
    size_t list_bytes = 5 + 1;
    for (const auto &d : config.devices)
        list_bytes += 1 + d.alias.size() + 1 + 6 + 1 + 16 + 1 + d.endpoint.size() + 1 + 2 + 1 + 32 + 1;
    if (list_bytes > 1024)
        throw ConfigError("the device list is too long for one ListDevices answer (" + std::to_string(list_bytes) +
                          " of 1024 bytes): use fewer devices or shorter aliases and endpoints");
}

std::string usage()
{
    return R"(usage: drivelink-agent [--config FILE] [flags]

Owns device ports and shares them over TCP. Flags override the config file.

flags:
  --config PATH              JSON config file (keys below)
  --bind ADDR                numeric IPv4/IPv6 listen address (default 127.0.0.1)
  --port N                   listen port, 0..65535 (default 47620; 0 = ephemeral)
  --token TOKEN              shared secret, 8..128 printable ASCII bytes
  --token-file PATH          read the token from the first line of PATH
  --device ALIAS=ENDPOINT[@BAUD]
                             add a device (repeatable; replaces the file's list)
                             ENDPOINT is serial:///dev/ttyACM0 or tcp://127.0.0.1:5000
                             BAUD is 9600..4000000 (serial only, default 921600)
  --hello-timeout-ms N       time allowed for a client's Hello (default 5000)
  --retry-ms N               device reconnect interval (default 2000)
  --request-timeout-ms N     device request timeout (default 3000)
  --history-size N           history entries kept per device, 16..4096 (default 256)
  --name NAME                mDNS instance name (default: the host name)
  --no-advertise             do not advertise over mDNS (only LAN binds advertise)
  --help                     show this text
  --version                  show the version
  Values may also be given as --flag=value. Durations are 100..60000.

config file keys (unknown keys are errors):
  bind, port, token, token_file (relative to the config file; token and
  token_file are mutually exclusive), hello_timeout_ms, retry_ms,
  request_timeout_ms, history_size, advertise (true/false), name (1..63 bytes,
  no '.'), devices: [{alias, endpoint, baud}]
  alias: 1..32 of a-z 0-9 _ -, starting with a letter, not 12 hex digits.
  1..8 devices with unique aliases and endpoints.

example:
  {
    "bind": "127.0.0.1",
    "port": 47620,
    "token_file": "agent.token",
    "devices": [
      {"alias": "base", "endpoint": "serial:///dev/ttyACM0", "baud": 921600},
      {"alias": "sim", "endpoint": "tcp://127.0.0.1:5000"}
    ]
  }
)";
}
} // namespace drivelink::agent
