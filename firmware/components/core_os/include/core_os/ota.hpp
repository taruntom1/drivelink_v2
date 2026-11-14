#pragma once
#include "core_os/types.hpp"
#include "drivelink/protocol/sha256.hpp"
#include <memory>
#include <span>

namespace core_os {
class Ota;
namespace native { void failOtaOperation(Ota &, const char *); void rebootOta(Ota &); }
struct OtaBootInfo {
    uint8_t running = 0, selected = 0;
    bool pending = false;
    uint32_t capacity = 0;
    drivelink::protocol::Sha256Digest elf{};
    std::array<char,33> version{};
};
// One config caller. The platform owns image validation and boot selection.
// Setup/BEGIN may allocate; all chunk scratch belongs to the caller.
class Ota {
public:
    Ota();
    ~Ota();
    Ota(const Ota &) = delete;
    Ota &operator=(const Ota &) = delete;
    Err info(OtaBootInfo &out);
    Err begin(uint32_t size);
    Err write(std::span<const uint8_t> bytes);
    Err read(uint32_t offset, std::span<uint8_t> bytes);
    Err finish(); // validates image, frees upload handle, does not select it
    Err select(); // selects validated candidate
    Err abort();  // closes handle and restores running boot slot
    Err confirm();
    struct Impl;
private:
    friend void native::failOtaOperation(Ota &, const char *);
    friend void native::rebootOta(Ota &);
    std::unique_ptr<Impl> m_impl;
};
}
