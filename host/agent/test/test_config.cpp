#include <doctest.h>

#include "config.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace drivelink::agent;
namespace fs = std::filesystem;

namespace
{
// True when `fn` throws ConfigError whose message contains `needle`.
template <class F> bool refuses(F fn, std::string_view needle)
{
    try
    {
        fn();
    }
    catch (const ConfigError &e)
    {
        return std::string_view(e.what()).find(needle) != std::string_view::npos;
    }
    return false;
}

bool refusesJson(std::string_view text, std::string_view needle)
{
    return refuses([&] { parseConfig(text); }, needle);
}

CommandLine run(std::vector<std::string_view> args, AgentConfig &config)
{
    return parseCommandLine(args, config);
}

bool refusesFlags(std::vector<std::string_view> args, std::string_view needle)
{
    AgentConfig c;
    return refuses([&] { run(args, c); }, needle);
}

struct TempDir
{
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / ("drivelink-agent-config-test-" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string &name, const std::string &content) const
    {
        const fs::path p = path / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
        return p;
    }
};

constexpr const char *kToken = "secret-token-1";

AgentConfig validConfig()
{
    AgentConfig c;
    c.token = kToken;
    c.devices.push_back({"base", "serial:///dev/ttyACM0", 921600});
    return c;
}

std::string deviceJson(const std::string &body)
{
    return "{\"devices\":[" + body + "]}";
}
} // namespace

TEST_CASE("config: defaults and a full valid file")
{
    AgentConfig d;
    CHECK(d.bind == "127.0.0.1");
    CHECK(d.port == 47620);
    CHECK(d.hello_timeout.count() == 5000);
    CHECK(d.retry.count() == 2000);
    CHECK(d.request_timeout.count() == 3000);
    CHECK(d.advertise);
    CHECK(d.name.empty());

    const auto c = parseConfig(R"({
        "bind": "0.0.0.0", "port": 1234, "token": "abcdefgh",
        "hello_timeout_ms": 100, "retry_ms": 60000, "request_timeout_ms": 250,
        "devices": [
            {"alias": "base", "endpoint": "serial:///dev/ttyACM0", "baud": 115200},
            {"alias": "sim_1", "endpoint": "tcp://127.0.0.1:5000"}
        ]})");
    CHECK(c.bind == "0.0.0.0");
    CHECK(c.port == 1234);
    CHECK(c.token == "abcdefgh");
    CHECK(c.hello_timeout.count() == 100);
    CHECK(c.retry.count() == 60000);
    CHECK(c.request_timeout.count() == 250);
    REQUIRE(c.devices.size() == 2);
    CHECK(c.devices[0].alias == "base");
    CHECK(c.devices[0].endpoint == "serial:///dev/ttyACM0");
    CHECK(c.devices[0].baud == 115200);
    CHECK(c.devices[1].endpoint == "tcp://127.0.0.1:5000");
    CHECK(c.devices[1].baud == 921600);
    CHECK_NOTHROW(validate(c));
}

TEST_CASE("config: an empty object needs neither devices nor a token to parse")
{
    const auto c = parseConfig("{}");
    CHECK(c.token.empty());
    CHECK(c.devices.empty());
    CHECK(refuses([&] { validate(c); }, "token"));
}

TEST_CASE("config: malformed JSON and wrong top-level types")
{
    CHECK_THROWS_AS(parseConfig("{"), ConfigError);
    CHECK_THROWS_AS(parseConfig(""), ConfigError);
    CHECK(refusesJson("[]", "object"));
    CHECK(refusesJson("42", "object"));
}

TEST_CASE("config: unknown keys are refused and named")
{
    CHECK(refusesJson(R"({"prot": 1})", "prot"));
    CHECK(refusesJson(deviceJson(R"({"alias":"a","endpoint":"serial:///x","speed":1})"), "speed"));
}

TEST_CASE("config: wrong JSON types are refused")
{
    CHECK(refusesJson(R"({"bind": 5})", "bind"));
    CHECK(refusesJson(R"({"port": "80"})", "port"));
    CHECK(refusesJson(R"({"port": 80.5})", "port"));
    CHECK(refusesJson(R"({"port": true})", "port"));
    CHECK(refusesJson(R"({"token": 12345678})", "token"));
    CHECK(refusesJson(R"({"token_file": 1})", "token_file"));
    CHECK(refusesJson(R"({"retry_ms": "500"})", "retry_ms"));
    CHECK(refusesJson(R"({"devices": {}})", "devices"));
    CHECK(refusesJson(R"({"devices": [1]})", "devices[0]"));
    CHECK(refusesJson(deviceJson(R"({"alias":1,"endpoint":"serial:///x"})"), "alias"));
    CHECK(refusesJson(deviceJson(R"({"alias":"a","endpoint":2})"), "endpoint"));
    CHECK(refusesJson(deviceJson(R"({"alias":"a","endpoint":"serial:///x","baud":"9600"})"), "baud"));
}

TEST_CASE("config: port range 0..65535")
{
    CHECK(parseConfig(R"({"port": 0})").port == 0);
    CHECK(parseConfig(R"({"port": 65535})").port == 65535);
    CHECK(refusesJson(R"({"port": 65536})", "port"));
    CHECK(refusesJson(R"({"port": -1})", "port"));
    CHECK(refusesJson(R"({"port": 99999999999999999999})", "port"));
}

TEST_CASE("config: bind must be a numeric address")
{
    CHECK(parseConfig(R"({"bind": "192.168.1.5"})").bind == "192.168.1.5");
    CHECK(parseConfig(R"({"bind": "::1"})").bind == "::1");
    CHECK(parseConfig(R"({"bind": "fe80::1"})").bind == "fe80::1");
    CHECK(refusesJson(R"({"bind": "localhost"})", "bind"));
    CHECK(refusesJson(R"({"bind": ""})", "bind"));
    CHECK(refusesJson(R"({"bind": "256.0.0.1"})", "bind"));
    CHECK(refusesJson(R"({"bind": "1.2.3"})", "bind"));
}

TEST_CASE("config: token length and characters")
{
    CHECK(parseConfig(R"({"token": "12345678"})").token == "12345678");
    CHECK(parseConfig("{\"token\": \"" + std::string(128, 'x') + "\"}").token.size() == 128);
    CHECK(refusesJson(R"({"token": "1234567"})", "token"));
    CHECK(refusesJson("{\"token\": \"" + std::string(129, 'x') + "\"}", "token"));
    CHECK(refusesJson(R"({"token": "has space1"})", "token"));
    CHECK(refusesJson("{\"token\": \"tab\\tchars1\"}", "token"));
    CHECK(refusesJson("{\"token\": \"caf\\u00e9-token\"}", "token"));
    CHECK(parseConfig(R"({"token": "!~!~!~!~"})").token == "!~!~!~!~");
}

TEST_CASE("config: durations 100..60000 ms")
{
    for (const char *key : {"hello_timeout_ms", "retry_ms", "request_timeout_ms"})
    {
        CAPTURE(key);
        const auto text = [&](int v) { return std::string("{\"") + key + "\": " + std::to_string(v) + "}"; };
        CHECK_NOTHROW(parseConfig(text(100)));
        CHECK_NOTHROW(parseConfig(text(60000)));
        CHECK(refusesJson(text(99), key));
        CHECK(refusesJson(text(60001), key));
        CHECK(refusesJson(text(0), key));
        CHECK(refusesJson(text(-5), key));
    }
}

TEST_CASE("config: history_size 16..4096, default 256, and its flag")
{
    CHECK(AgentConfig{}.history_size == 256);
    CHECK(parseConfig(R"({"history_size": 16})").history_size == 16);
    CHECK(parseConfig(R"({"history_size": 4096})").history_size == 4096);
    CHECK(refusesJson(R"({"history_size": 15})", "history_size"));
    CHECK(refusesJson(R"({"history_size": 4097})", "history_size"));
    AgentConfig c;
    std::vector<std::string_view> args{"--history-size", "1000"};
    parseCommandLine(args, c);
    CHECK(c.history_size == 1000);
    std::vector<std::string_view> bad{"--history-size=5000"};
    CHECK_THROWS_AS(parseCommandLine(bad, c), ConfigError);
}

TEST_CASE("config: advertise is a boolean, default true")
{
    CHECK(AgentConfig{}.advertise);
    CHECK(parseConfig("{}").advertise);
    CHECK(parseConfig(R"({"advertise": true})").advertise);
    CHECK_FALSE(parseConfig(R"({"advertise": false})").advertise);
    CHECK(refusesJson(R"({"advertise": 0})", "advertise"));
    CHECK(refusesJson(R"({"advertise": 1})", "advertise"));
    CHECK(refusesJson(R"({"advertise": "false"})", "advertise"));
    CHECK(refusesJson(R"({"advertise": null})", "advertise"));
}

TEST_CASE("config: mDNS instance name rules")
{
    const auto nameJson = [](const std::string &name) { return "{\"name\": \"" + name + "\"}"; };
    CHECK(AgentConfig{}.name.empty());
    CHECK(parseConfig("{}").name.empty());
    CHECK(parseConfig(nameJson("robot-pi")).name == "robot-pi");
    CHECK(parseConfig(nameJson("Bench agent 2")).name == "Bench agent 2");
    const std::string umlaut = "Roboter-\xC3\x9C"; // U with a diaeresis, UTF-8
    CHECK(parseConfig(nameJson(umlaut)).name == umlaut);
    CHECK(parseConfig(nameJson(std::string(63, 'a'))).name.size() == 63);

    CHECK(refusesJson(R"({"name": 5})", "name"));
    // An explicit empty name is refused; leaving the key out is the default.
    CHECK(refusesJson(R"({"name": ""})", "name"));
    CHECK(refusesJson(R"({"name": ""})", "1..63"));
    CHECK(refusesJson(nameJson(std::string(64, 'a')), "name"));
    CHECK(refusesJson(nameJson(std::string(64, 'a')), "1..63"));
    CHECK(refusesJson(nameJson("a.b"), "name"));
    CHECK(refusesJson(R"({"name": "a\u0007b"})", "name")); // BEL as a JSON escape
    CHECK(refusesJson(R"({"name": "a\u007fb"})", "name"));  // DEL
    // A lone UTF-8 continuation byte: the JSON parser refuses the text itself.
    CHECK_THROWS_AS(parseConfig(nameJson("bad\x80" "name")), ConfigError);
}

TEST_CASE("command line: --name and --no-advertise")
{
    AgentConfig c = parseConfig(R"({"name": "from-file"})");
    run({"--name", "from-flag"}, c);
    CHECK(c.name == "from-flag");
    CHECK(c.advertise);

    AgentConfig e;
    run({"--name=Bench agent 2", "--no-advertise"}, e);
    CHECK(e.name == "Bench agent 2");
    CHECK_FALSE(e.advertise);

    // --no-advertise overrides a file that advertises, and takes no value.
    AgentConfig f = parseConfig(R"({"advertise": true})");
    run({"--no-advertise"}, f);
    CHECK_FALSE(f.advertise);
    CHECK(refusesFlags({"--no-advertise=1"}, "--no-advertise"));
    CHECK(refusesFlags({"--no-advertise", "x"}, "x"));

    CHECK(refusesFlags({"--name"}, "--name"));
    CHECK(refusesFlags({"--name", ""}, "--name"));
    CHECK(refusesFlags({"--name", "a.b"}, "--name"));
    CHECK(refusesFlags({"--name", "a\x01" "b"}, "--name"));
    CHECK(refusesFlags({"--name", "bad\x80" "name"}, "--name"));
    const std::string too_long(64, 'x');
    CHECK(refusesFlags({"--name", too_long}, "1..63"));
}

TEST_CASE("config: device alias rules")
{
    const auto dev = [](const std::string &alias) {
        return deviceJson("{\"alias\":\"" + alias + "\",\"endpoint\":\"serial:///dev/x\"}");
    };
    CHECK_NOTHROW(parseConfig(dev("a")));
    CHECK_NOTHROW(parseConfig(dev("robot-1_left")));
    CHECK_NOTHROW(parseConfig(dev(std::string(32, 'a'))));
    CHECK(refusesJson(dev(""), "alias"));
    CHECK(refusesJson(dev(std::string(33, 'a')), "alias"));
    CHECK(refusesJson(dev("1abc"), "alias"));
    CHECK(refusesJson(dev("-abc"), "alias"));
    CHECK(refusesJson(dev("_abc"), "alias"));
    CHECK(refusesJson(dev("Abc"), "alias"));
    CHECK(refusesJson(dev("a b"), "alias"));
    CHECK(refusesJson(dev("a.b"), "alias"));
    // Exactly 12 hex digits could be mistaken for a device id.
    CHECK(refusesJson(dev("abcdef012345"), "device id"));
    CHECK(refusesJson(dev("aaaaaaaaaaaa"), "device id"));
    // Near misses are fine.
    CHECK_NOTHROW(parseConfig(dev("abcdef01234")));
    CHECK_NOTHROW(parseConfig(dev("abcdef0123456")));
    CHECK_NOTHROW(parseConfig(dev("abcdef01234g")));
}

TEST_CASE("config: device endpoint rules")
{
    const auto dev = [](const std::string &endpoint) {
        return deviceJson("{\"alias\":\"d\",\"endpoint\":\"" + endpoint + "\"}");
    };
    CHECK_NOTHROW(parseConfig(dev("serial:///dev/ttyACM0")));
    CHECK_NOTHROW(parseConfig(dev("serial://COM3")));
    CHECK_NOTHROW(parseConfig(dev("tcp://127.0.0.1:1")));
    CHECK_NOTHROW(parseConfig(dev("tcp://10.1.2.3:65535")));
    CHECK(refusesJson(dev("serial://"), "endpoint"));
    CHECK(refusesJson(dev(""), "endpoint"));
    CHECK(refusesJson(dev("/dev/ttyACM0"), "endpoint"));
    CHECK(refusesJson(dev("http://127.0.0.1:80"), "endpoint"));
    CHECK(refusesJson(dev("tcp://"), "tcp"));
    CHECK(refusesJson(dev("tcp://127.0.0.1"), "tcp"));
    CHECK(refusesJson(dev("tcp://localhost:5000"), "IPv4"));
    CHECK(refusesJson(dev("tcp://::1:5000"), "IPv4"));
    CHECK(refusesJson(dev("tcp://127.0.0.1:0"), "port"));
    CHECK(refusesJson(dev("tcp://127.0.0.1:65536"), "port"));
    CHECK(refusesJson(dev("tcp://127.0.0.1:"), "port"));
    CHECK(refusesJson(dev("tcp://127.0.0.1:80x"), "port"));
    CHECK(refusesJson(dev("tcp://127.0.0.1:-1"), "port"));
}

TEST_CASE("config: device baud rules")
{
    const auto dev = [](const std::string &endpoint, const std::string &baud) {
        return deviceJson("{\"alias\":\"d\",\"endpoint\":\"" + endpoint + "\",\"baud\":" + baud + "}");
    };
    CHECK(parseConfig(dev("serial:///x", "9600")).devices[0].baud == 9600);
    CHECK(parseConfig(dev("serial:///x", "4000000")).devices[0].baud == 4000000);
    CHECK(refusesJson(dev("serial:///x", "9599"), "baud"));
    CHECK(refusesJson(dev("serial:///x", "4000001"), "baud"));
    CHECK(refusesJson(dev("serial:///x", "0"), "baud"));
    CHECK(refusesJson(dev("serial:///x", "115200.5"), "baud"));
    CHECK(refusesJson(dev("tcp://127.0.0.1:5000", "921600"), "baud"));
    CHECK(parseConfig(deviceJson(R"({"alias":"d","endpoint":"serial:///x"})")).devices[0].baud == 921600);
}

TEST_CASE("config: device required keys")
{
    CHECK(refusesJson(deviceJson(R"({"endpoint":"serial:///x"})"), "alias"));
    CHECK(refusesJson(deviceJson(R"({"alias":"a"})"), "endpoint"));
}

TEST_CASE("config: token_file in the config file")
{
    TempDir tmp;
    tmp.write("agent.token", "file-token-123\n");
    const auto c = parseConfig(R"({"token_file": "agent.token"})", tmp.path.string());
    CHECK(c.token == "file-token-123");

    SUBCASE("relative to base_dir including subfolders")
    {
        tmp.write("sub/t.txt", "nested-token-1");
        CHECK(parseConfig(R"({"token_file": "sub/t.txt"})", tmp.path.string()).token == "nested-token-1");
    }
    SUBCASE("an absolute path is used as is")
    {
        const auto abs = tmp.write("abs.token", "absolute-token-1");
        const std::string json = "{\"token_file\": \"" + abs.generic_string() + "\"}";
        CHECK(parseConfig(json, "/nonexistent").token == "absolute-token-1");
    }
    SUBCASE("only the first line, trailing CR, LF and spaces stripped")
    {
        tmp.write("crlf.token", "crlf-token-12 \r\nsecond line\r\n");
        CHECK(parseConfig(R"({"token_file": "crlf.token"})", tmp.path.string()).token == "crlf-token-12");
        tmp.write("nonl.token", "no-newline-tok");
        CHECK(parseConfig(R"({"token_file": "nonl.token"})", tmp.path.string()).token == "no-newline-tok");
        tmp.write("spaces.token", "spaced-token-1   \n");
        CHECK(parseConfig(R"({"token_file": "spaces.token"})", tmp.path.string()).token == "spaced-token-1");
    }
    SUBCASE("a missing file is refused")
    {
        CHECK(refuses([&] { parseConfig(R"({"token_file": "nope"})", tmp.path.string()); }, "token_file"));
    }
    SUBCASE("an empty or invalid token file is refused")
    {
        tmp.write("empty.token", "");
        CHECK(refuses([&] { parseConfig(R"({"token_file": "empty.token"})", tmp.path.string()); }, "token_file"));
        tmp.write("short.token", "short\n");
        CHECK(refuses([&] { parseConfig(R"({"token_file": "short.token"})", tmp.path.string()); }, "token_file"));
        tmp.write("inner.token", "has a space inside\n");
        CHECK(refuses([&] { parseConfig(R"({"token_file": "inner.token"})", tmp.path.string()); }, "token_file"));
        tmp.write("blank1.token", "\nreal-token-123\n");
        CHECK(refuses([&] { parseConfig(R"({"token_file": "blank1.token"})", tmp.path.string()); }, "token_file"));
    }
    SUBCASE("token and token_file are mutually exclusive")
    {
        CHECK(refuses([&] { parseConfig(R"({"token":"abcdefgh","token_file":"agent.token"})", tmp.path.string()); },
                      "mutually exclusive"));
        CHECK(refuses([&] { parseConfig(R"({"token_file":"agent.token","token":"abcdefgh"})", tmp.path.string()); },
                      "mutually exclusive"));
    }
}

TEST_CASE("config: loadConfig resolves token_file against the file's directory")
{
    TempDir tmp;
    tmp.write("agent.token", "dir-token-1234\n");
    const auto cfg = tmp.write("conf/agent.json", R"({"token_file": "../agent.token", "port": 99,
        "devices": [{"alias": "a", "endpoint": "serial:///dev/x"}]})");
    const auto c = loadConfig(cfg.string());
    CHECK(c.token == "dir-token-1234");
    CHECK(c.port == 99);
    CHECK_NOTHROW(validate(c));
    CHECK(refuses([&] { loadConfig((tmp.path / "missing.json").string()); }, "missing.json"));
}

TEST_CASE("config: findConfigPath")
{
    const auto find = [](std::vector<std::string_view> a) { return findConfigPath(a); };
    CHECK(find({}) == std::nullopt);
    CHECK(find({"--port", "1"}) == std::nullopt);
    CHECK(find({"--config", "a.json"}) == std::optional<std::string>("a.json"));
    CHECK(find({"--config=b.json"}) == std::optional<std::string>("b.json"));
    CHECK(find({"--port", "1", "--config", "c.json", "--retry-ms", "200"}) == std::optional<std::string>("c.json"));
    CHECK(find({"--config"}) == std::nullopt);
}

TEST_CASE("command line: every flag overrides the config, both forms")
{
    AgentConfig c = parseConfig(R"({"bind":"0.0.0.0","port":1111,"token":"filetoken1",
        "hello_timeout_ms":111,"retry_ms":222,"request_timeout_ms":333})");
    const auto cl = run({"--bind", "::1", "--port", "2222", "--token", "flagtoken1", "--hello-timeout-ms", "1000",
                         "--retry-ms", "2000", "--request-timeout-ms", "3000"},
                        c);
    CHECK_FALSE(cl.help);
    CHECK_FALSE(cl.version);
    CHECK_FALSE(cl.config_path);
    CHECK(c.bind == "::1");
    CHECK(c.port == 2222);
    CHECK(c.token == "flagtoken1");
    CHECK(c.hello_timeout.count() == 1000);
    CHECK(c.retry.count() == 2000);
    CHECK(c.request_timeout.count() == 3000);

    AgentConfig e;
    run({"--bind=10.0.0.1", "--port=0", "--token=eqtoken123", "--hello-timeout-ms=100", "--retry-ms=60000",
         "--request-timeout-ms=4000"},
        e);
    CHECK(e.bind == "10.0.0.1");
    CHECK(e.port == 0);
    CHECK(e.token == "eqtoken123");
    CHECK(e.hello_timeout.count() == 100);
    CHECK(e.retry.count() == 60000);
    CHECK(e.request_timeout.count() == 4000);
}

TEST_CASE("command line: untouched settings stay as the file set them")
{
    AgentConfig c = validConfig();
    c.port = 5555;
    run({"--retry-ms", "700"}, c);
    CHECK(c.port == 5555);
    CHECK(c.token == kToken);
    CHECK(c.retry.count() == 700);
    CHECK(c.devices.size() == 1);
}

TEST_CASE("command line: help, version and config recorded")
{
    AgentConfig c;
    CHECK(run({"--help"}, c).help);
    CHECK(run({"--version"}, c).version);
    const auto both = run({"--help", "--version", "--config", "x.json"}, c);
    CHECK(both.help);
    CHECK(both.version);
    CHECK(both.config_path == std::optional<std::string>("x.json"));
    CHECK(run({"--config=y.json"}, c).config_path == std::optional<std::string>("y.json"));
    CHECK(run({}, c).config_path == std::nullopt);
    CHECK(refusesFlags({"--help=1"}, "--help"));
    CHECK(refusesFlags({"--config", ""}, "--config"));
}

TEST_CASE("command line: unknown flags, stray arguments and missing values")
{
    CHECK(refusesFlags({"--frobnicate"}, "--frobnicate"));
    CHECK(refusesFlags({"--frobnicate=1"}, "--frobnicate"));
    CHECK(refusesFlags({"-p", "1"}, "-p"));
    CHECK(refusesFlags({"extra"}, "extra"));
    for (const char *flag : {"--config", "--bind", "--port", "--token", "--token-file", "--device",
                             "--hello-timeout-ms", "--retry-ms", "--request-timeout-ms", "--history-size", "--name"})
    {
        CAPTURE(flag);
        CHECK(refusesFlags({flag}, flag));
    }
    CHECK(refusesFlags({"--port", "1", "--bind"}, "--bind"));
}

TEST_CASE("command line: values are range-checked")
{
    CHECK(refusesFlags({"--port", "65536"}, "--port"));
    CHECK(refusesFlags({"--port", "-1"}, "--port"));
    CHECK(refusesFlags({"--port", "abc"}, "--port"));
    CHECK(refusesFlags({"--port", ""}, "--port"));
    CHECK(refusesFlags({"--port=1x"}, "--port"));
    CHECK(refusesFlags({"--bind", "localhost"}, "--bind"));
    CHECK(refusesFlags({"--token", "short"}, "--token"));
    CHECK(refusesFlags({"--token", "has space1"}, "--token"));
    for (const char *flag : {"--hello-timeout-ms", "--retry-ms", "--request-timeout-ms"})
    {
        CAPTURE(flag);
        CHECK(refusesFlags({flag, "99"}, flag));
        CHECK(refusesFlags({flag, "60001"}, flag));
        CHECK(refusesFlags({flag, "x"}, flag));
        AgentConfig c;
        CHECK_NOTHROW(run({flag, "100"}, c));
        CHECK_NOTHROW(run({flag, "60000"}, c));
    }
}

TEST_CASE("command line: --device forms")
{
    AgentConfig c;
    run({"--device", "base=serial:///dev/ttyACM0"}, c);
    REQUIRE(c.devices.size() == 1);
    CHECK(c.devices[0].alias == "base");
    CHECK(c.devices[0].endpoint == "serial:///dev/ttyACM0");
    CHECK(c.devices[0].baud == 921600);

    AgentConfig b;
    run({"--device=base=serial:///dev/ttyACM0@115200"}, b);
    REQUIRE(b.devices.size() == 1);
    CHECK(b.devices[0].alias == "base");
    CHECK(b.devices[0].endpoint == "serial:///dev/ttyACM0");
    CHECK(b.devices[0].baud == 115200);

    AgentConfig t;
    run({"--device", "sim=tcp://127.0.0.1:5000"}, t);
    CHECK(t.devices[0].endpoint == "tcp://127.0.0.1:5000");

    // The baud suffix is split at the last '@'.
    AgentConfig at;
    run({"--device", "odd=serial:///dev/a@b@9600"}, at);
    CHECK(at.devices[0].endpoint == "serial:///dev/a@b");
    CHECK(at.devices[0].baud == 9600);
}

TEST_CASE("command line: --device refusals")
{
    CHECK(refusesFlags({"--device", "noequals"}, "--device"));
    CHECK(refusesFlags({"--device", "=serial:///x"}, "alias"));
    CHECK(refusesFlags({"--device", "Bad=serial:///x"}, "alias"));
    CHECK(refusesFlags({"--device", "abcdef012345=serial:///x"}, "device id"));
    CHECK(refusesFlags({"--device", "a="}, "endpoint"));
    CHECK(refusesFlags({"--device", "a=serial://"}, "endpoint"));
    CHECK(refusesFlags({"--device", "a=ftp://x"}, "endpoint"));
    CHECK(refusesFlags({"--device", "a=serial:///x@9599"}, "baud"));
    CHECK(refusesFlags({"--device", "a=serial:///x@4000001"}, "baud"));
    CHECK(refusesFlags({"--device", "a=serial:///x@fast"}, "baud"));
    CHECK(refusesFlags({"--device", "a=serial:///x@"}, "baud"));
    CHECK(refusesFlags({"--device", "a=tcp://127.0.0.1:5000@9600"}, "baud"));
    CHECK(refusesFlags({"--device", "a=tcp://host:5000"}, "IPv4"));
    CHECK(refusesFlags({"--device", "a=tcp://127.0.0.1:0"}, "port"));
}

TEST_CASE("command line: --device replaces the file's list, repeats accumulate")
{
    AgentConfig c = parseConfig(deviceJson(
        R"({"alias":"file1","endpoint":"serial:///dev/f1"},{"alias":"file2","endpoint":"serial:///dev/f2"})"));
    REQUIRE(c.devices.size() == 2);
    run({"--device", "one=serial:///dev/a", "--device=two=tcp://127.0.0.1:9", "--device", "three=serial:///dev/c@9600"}, c);
    REQUIRE(c.devices.size() == 3);
    CHECK(c.devices[0].alias == "one");
    CHECK(c.devices[1].alias == "two");
    CHECK(c.devices[2].alias == "three");
    CHECK(c.devices[2].baud == 9600);

    // Without --device the file's list is kept.
    AgentConfig k = parseConfig(deviceJson(R"({"alias":"file1","endpoint":"serial:///dev/f1"})"));
    run({"--port", "1"}, k);
    CHECK(k.devices.size() == 1);
}

TEST_CASE("command line: --token-file reads a file and overrides the token")
{
    TempDir tmp;
    const auto tf = tmp.write("t.token", "flag-file-token\r\n");
    AgentConfig c = validConfig();
    run({"--token-file", tf.string()}, c);
    CHECK(c.token == "flag-file-token");

    AgentConfig e = validConfig();
    run({"--token-file=" + tf.string()}, e);
    CHECK(e.token == "flag-file-token");

    // A flag overrides a token_file from the config file.
    tmp.write("cfg.token", "from-config-file");
    AgentConfig f = parseConfig(R"({"token_file": "cfg.token"})", tmp.path.string());
    CHECK(f.token == "from-config-file");
    run({"--token", "from-the-flag"}, f);
    CHECK(f.token == "from-the-flag");

    // A missing or bad file is refused.
    AgentConfig m;
    CHECK(refuses([&] { run({"--token-file", (tmp.path / "none").string()}, m); }, "--token-file"));
    const auto bad = tmp.write("bad.token", "x\n");
    CHECK(refuses([&] { run({"--token-file", bad.string()}, m); }, "--token-file"));

    // The two token flags together are refused.
    AgentConfig both;
    CHECK(refuses([&] { run({"--token", "abcdefgh", "--token-file", tf.string()}, both); }, "mutually exclusive"));
    CHECK(refuses([&] { run({"--token-file", tf.string(), "--token", "abcdefgh"}, both); }, "mutually exclusive"));
}

TEST_CASE("validate: token, device count, uniqueness")
{
    CHECK_NOTHROW(validate(validConfig()));

    AgentConfig noToken = validConfig();
    noToken.token.clear();
    CHECK(refuses([&] { validate(noToken); }, "token"));

    AgentConfig badToken = validConfig();
    badToken.token = "short";
    CHECK(refuses([&] { validate(badToken); }, "token"));

    AgentConfig noDevices = validConfig();
    noDevices.devices.clear();
    CHECK(refuses([&] { validate(noDevices); }, "device"));

    AgentConfig eight = validConfig();
    eight.devices.clear();
    for (int i = 0; i < 8; ++i)
        eight.devices.push_back({"d" + std::to_string(i), "serial:///dev/ttyACM" + std::to_string(i), 921600});
    CHECK_NOTHROW(validate(eight));
    eight.devices.push_back({"d8", "serial:///dev/ttyACM8", 921600});
    CHECK(refuses([&] { validate(eight); }, "8"));

    AgentConfig dupAlias = validConfig();
    dupAlias.devices.push_back({"base", "serial:///dev/ttyACM1", 921600});
    CHECK(refuses([&] { validate(dupAlias); }, "alias"));

    AgentConfig dupEndpoint = validConfig();
    dupEndpoint.devices.push_back({"other", "serial:///dev/ttyACM0", 115200});
    CHECK(refuses([&] { validate(dupEndpoint); }, "endpoint"));
}

TEST_CASE("validate: flags can supply what the file lacks")
{
    AgentConfig c = parseConfig("{}");
    run({"--token", "abcdefgh", "--device", "a=serial:///dev/x"}, c);
    CHECK_NOTHROW(validate(c));
}

TEST_CASE("config: loopback()")
{
    const auto lo = [](const char *bind) {
        AgentConfig c;
        c.bind = bind;
        return c.loopback();
    };
    CHECK(lo("127.0.0.1"));
    CHECK(lo("127.1.2.3"));
    CHECK(lo("127.255.255.255"));
    CHECK(lo("::1"));
    CHECK(lo("0:0:0:0:0:0:0:1"));
    CHECK_FALSE(lo("0.0.0.0"));
    CHECK_FALSE(lo("128.0.0.1"));
    CHECK_FALSE(lo("126.255.255.255"));
    CHECK_FALSE(lo("192.168.1.2"));
    CHECK_FALSE(lo("::"));
    CHECK_FALSE(lo("::2"));
    CHECK_FALSE(lo("fe80::1"));
    CHECK_FALSE(lo("localhost"));
    CHECK(AgentConfig{}.loopback());
}

TEST_CASE("config: usage lists every flag and config key")
{
    const std::string u = usage();
    for (const char *s : {"--config", "--bind", "--port", "--token", "--token-file", "--device", "--hello-timeout-ms",
                          "--retry-ms", "--request-timeout-ms", "--help", "--version", "token_file",
                          "hello_timeout_ms", "retry_ms", "request_timeout_ms", "devices", "alias", "endpoint", "baud",
                          "--name", "--no-advertise", "advertise", "name"})
    {
        CAPTURE(s);
        CHECK(u.find(s) != std::string::npos);
    }
}

TEST_CASE("validate: the device list must fit one ListDevices answer")
{
    drivelink::agent::AgentConfig config;
    config.token = "0123456789abcdef";
    const std::string long_path(170, 'x');
    for (int i = 0; i < 4; ++i)
        config.devices.push_back({"dev" + std::to_string(i), "serial:///dev/" + long_path + std::to_string(i), 921600});
    CHECK_NOTHROW(drivelink::agent::validate(config));
    config.devices.push_back({"dev4", "serial:///dev/" + long_path + "4", 921600});
    CHECK_THROWS_AS(drivelink::agent::validate(config), drivelink::agent::ConfigError);
}
