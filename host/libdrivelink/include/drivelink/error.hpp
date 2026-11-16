#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

namespace drivelink
{
enum class ErrorKind
{
    Connection,
    Timeout,
    Protocol,
    Rejected,
    Usage,
    Cancelled
};
// Metadata lives in the base so copying Error (future async completions) keeps
// the device refusal and uncertainty. A diagnostic query uses family zero.
class Error : public std::runtime_error
{
  public:
    Error(ErrorKind kind, const std::string &message, uint8_t result = 0, uint8_t detail = 0)
        : std::runtime_error(message), kind(kind), result(result), detail(detail)
    {
    }
    void setMessage(const std::string &message)
    {
        std::runtime_error::operator=(std::runtime_error(message));
    }
    ErrorKind kind;
    uint8_t result = 0, detail = 0, family = 0, op = 0;
    bool has_request_context = false;
    // True when a submitted mutation may have taken effect. Includes queued
    // writes that can still reach the peer; false for local preflight failures.
    bool outcome_unknown = false;
    // An observed Boot invalidated this request's device generation. Distinct
    // from malformed protocol data; readiness polling can restart safely.
    bool boot_changed = false;
    // Nonzero when the peer answered with this incompatible wire version
    // (core or diag), so a client can show both versions.
    uint8_t peer_version = 0;
    // The operating-system error code when opening the endpoint failed
    // (Win32 ERROR_ACCESS_DENIED for a serial port another program holds).
    int os_error = 0;
};
class DeviceError : public Error
{
  public:
    DeviceError(const std::string &message, uint8_t result, uint8_t detail, uint8_t family, uint8_t op)
        : Error(ErrorKind::Rejected, message, result, detail)
    {
        this->family = family;
        this->op = op;
        has_request_context = true;
    }
};
} // namespace drivelink
