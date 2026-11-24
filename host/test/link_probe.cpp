#include "drivelink/link.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// Public-API consumer of drivelink/link.hpp and of baud auto-detection, for
// link_integration.py and for the link-trial bench procedure (docs/bench.md).
// The TSV is a test protocol, not a CLI output format.
namespace {
using namespace drivelink;
constexpr const char *kUsage =
    "link_probe ENDPOINT [--baud N|auto] [--last-baud N] [--detect-ms N] [--timeout-ms N] [--window-ms N] COMMAND\n"
    "  state                       read the link\n"
    "  confirm                     confirm a running trial\n"
    "  trial BAUD TX RX            stage, SAVE, REBOOT, follow and confirm the trial\n"
    "  watch BAUD TX RX            the same, but let the trial revert\n"
    "  start BAUD TX RX            stage, SAVE and REBOOT only\n";
std::string text(const LinkSettings &link)
{
    return std::to_string(link.baud) + ' ' + std::to_string(link.tx_pin) + ' ' + std::to_string(link.rx_pin);
}
void print(const LinkState &state)
{
    std::cout << "DEVICE\t" << hex(state.info.device_id) << "\nRUNNING\t" << text(state.running) << "\nNEXT\t"
              << text(state.next) << "\nTRIAL\t" << state.info.link_trial_ms << '\n';
}
const char *name(LinkOutcome outcome)
{
    static constexpr const char *kNames[]{"Confirmed", "Unconfirmed", "Reverted", "NotRebooted",
                                          "Deferred", "WrongDevice", "OtherLink", "NotReachable"};
    return kNames[static_cast<unsigned>(outcome)];
}
const char *name(LinkConfirmation confirmation)
{
    static constexpr const char *kNames[]{"Confirmed", "Unconfirmed", "NoTrial", "Reverted", "WrongDevice"};
    return kNames[static_cast<unsigned>(confirmation)];
}
void print(const LinkTrialResult &result)
{
    std::cout << "OUTCOME\t" << name(result.outcome) << "\nBAUD\t" << result.baud << '\n';
    if (result.state) { print(*result.state); }
    if (!result.problem.empty()) { std::cout << "PROBLEM\t" << result.problem << '\n'; }
    std::cout << std::flush;
}
int run(const std::vector<std::string> &args)
{
    SessionOptions options;
    LinkFollowOptions follow;
    size_t at = 1;
    auto number = [&](const std::string &value) { return parseUnsigned(value, 4000000); };
    for (; at + 1 < args.size() && args[at].starts_with("--"); at += 2)
    {
        const auto &key = args[at], &value = args[at + 1];
        if (key == "--baud") { options.baud = value == "auto" ? kBaudAuto : number(value); }
        else if (key == "--last-baud") { options.last_baud = number(value); }
        else if (key == "--detect-ms") { options.detect_timeout = std::chrono::milliseconds(number(value)); }
        else if (key == "--timeout-ms") { options.timeout = std::chrono::milliseconds(number(value)); }
        else if (key == "--window-ms") { follow.window = std::chrono::milliseconds(number(value)); }
        else { std::cerr << kUsage; return 2; }
    }
    if (at >= args.size()) { std::cerr << kUsage; return 2; }
    const auto &command = args[at];
    const bool change = command == "trial" || command == "watch" || command == "start";
    if (args.size() != at + (change ? 4U : 1U) || (!change && command != "state" && command != "confirm"))
    {
        std::cerr << kUsage;
        return 2;
    }
    std::optional<LinkExpectation> expected;
    {
        Session session(args[0], options);
        Client client(session);
        std::cout << "BAUD\t" << session.baud() << '\n';
        const auto state = linkState(client);
        print(state);
        std::cout << std::flush;
        if (command == "state") { return 0; }
        if (command == "confirm")
        {
            std::cout << "CONFIRM\t" << name(confirmLinkTrial(client, state.info.device_id)) << '\n';
            return 0;
        }
        (void)stageLink(client, {number(args[at + 1]), static_cast<uint8_t>(parseUnsigned(args[at + 2], 255)),
                                 static_cast<uint8_t>(parseUnsigned(args[at + 3], 255))});
        client.system(static_cast<uint8_t>(protocol::defs::SystemOp::Save));
        expected = expectLinkTrial(linkState(client));
        if (!expected) { throw Error(ErrorKind::Usage, "the link is unchanged"); }
        client.system(static_cast<uint8_t>(protocol::defs::SystemOp::Reboot));
        std::cout << "REBOOT\t" << text(expected->trial) << '\n' << std::flush;
    }
    if (command == "start") { return 0; }
    follow.confirm = command == "trial";
    for (;;)
    {
        const auto result = followLinkTrial(args[0], options, *expected, follow);
        print(result);
        if (result.outcome != LinkOutcome::Unconfirmed || command != "watch") { return 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        const std::vector<std::string> args(argv + 1, argv + argc);
        if (args.empty()) { std::cerr << kUsage; return 2; }
        return run(args);
    }
    catch (const Error &e)
    {
        std::cout << "ERROR\t" << static_cast<int>(e.kind) << '\t' << unsigned(e.result) << '\t' << unsigned(e.detail)
                  << '\t' << e.what() << '\n';
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
