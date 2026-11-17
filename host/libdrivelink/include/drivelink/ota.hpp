#pragma once
#include "drivelink/client.hpp"
#include <chrono>
#include <functional>
#include <string>

// The OTA update workflow after the upload: reboot, reconnect, identity check
// and CONFIRM ([ota.md](../../../../docs/protocol/ota.md)). The CLI runs it
// synchronously through otaUpdate(); the GUI runs the same steps on its own
// connection, which owns the reboot expectation and the reconnect window.
namespace drivelink {
// A validated unsigned ESP32-S3 application image and its app descriptor.
struct OtaImage
{
    Bytes bytes;
    protocol::Sha256Digest elf_sha256{}; // the build identity (esp_app_desc_t)
    protocol::Sha256Digest sha256{};     // the whole file, as BEGIN declares it
    std::string version, project, date, time, idf_version;
    std::string buildId() const; // first eight ELF bytes in hex, as GET_INFO reports
};
// Throws Usage for a file that is not a complete application .bin.
OtaImage describeOtaImage(Bytes bytes);
OtaImage loadOtaImage(const std::string &utf8_path);

// What the device runs after the candidate's reboot, compared with the
// candidate. Only Confirmable and Confirmed match the uploaded build.
enum class OtaBoot
{
    Confirmable,  // the candidate runs, pending confirmation
    Confirmed,    // the candidate runs and is already confirmed
    NotCommitted, // the old slot still runs with the image in Ready/Error
    RolledBack,   // the old slot runs again, idle: the candidate never stayed
    Mismatch      // anything else; never confirm
};
struct OtaCandidate
{
    protocol::Sha256Digest elf_sha256{};
    uint8_t previous_slot = 0; // running slot before the upload
};
OtaBoot classifyOtaBoot(const OtaCandidate &candidate, const DeviceInfo &info, const OtaStatus &status);
// The CLI's message for a verdict that must not be confirmed (empty otherwise).
std::string otaBootProblem(OtaBoot verdict);

// REBOOT after FINISH. A reply is acceptance only; a timeout, a lost link or a
// Boot that interrupted the request leaves the outcome uncertain. Refusals and
// other errors throw. Never call this twice for one update.
enum class OtaReboot { Accepted, Uncertain };
OtaReboot requestOtaReboot(Client &client);

// Reads GET_INFO (core version check) and STATUS, classifies them and, only
// for Confirmable with `confirm`, sends CONFIRM once and rechecks the identity.
// A timed-out CONFIRM is reconciled through STATUS: still pending throws
// Timeout ("confirmation uncertain", with outcome_unknown and family/op of
// CONFIRM); a changed identity throws Protocol.
// `cancelled` is asked just before CONFIRM and throws Usage when true.
struct OtaConfirmation
{
    OtaBoot verdict = OtaBoot::Mismatch;
    DeviceInfo info;
    OtaStatus status;
    bool confirmed = false; // the running candidate is confirmed now
};
OtaConfirmation confirmOtaCandidate(Client &client, const OtaCandidate &candidate, bool confirm,
                                    const std::function<bool()> &cancelled = {});

// The whole synchronous workflow on its own sessions (the CLI's ota-upload):
// upload and FINISH, REBOOT once, reconnect for `reconnect_window`, identity
// check, CONFIRM. Throws with the CLI's recovery messages. As the CLI always
// did, an uncertain CONFIRM is reconciled within the window by reconnecting
// and checking again; CONFIRM is idempotent. REBOOT is never repeated.
struct OtaUpdateOptions
{
    bool upload_only = false;
    bool confirm = true;
    uint8_t window = 4;
    std::chrono::milliseconds reconnect_window{30000};
};
struct OtaUpdateResult
{
    OtaStatus status;
    bool rebooted = false;
    bool confirmed = false;
};
OtaUpdateResult otaUpdate(const std::string &endpoint, const SessionOptions &options, const OtaImage &image,
                          const OtaUpdateOptions &update, const std::function<bool(const OtaProgress &)> &progress = {},
                          const std::function<bool()> &cancelled = {});
} // namespace drivelink
