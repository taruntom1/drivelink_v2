#pragma once
#include "core_os/ota.hpp"
#include <filesystem>

namespace core_os::native {
// Vdev only: set before constructing Core. Empty means an isolated baseline.
void setOtaDirectory(const std::filesystem::path &directory);
// Seed a new vdev state from its compiled source build ID before Core exists.
void setOtaBaseline(const drivelink::protocol::Sha256Digest &elf,const char *version);
// Deterministic tests: fail a backend operation before it takes effect.
// Operation names: info, begin, write, read, finish, select, abort, confirm.
// select_after reports failure after durable selection, modeling uncertainty.
void failOtaOperation(Ota &ota, const char *operation);
void rebootOta(Ota &ota);
// What otaImageUnconfirmed() answers at least until it is called with false.
void setOtaImageUnconfirmed(bool unconfirmed);
}
