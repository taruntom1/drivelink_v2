#pragma once
#include "core_os/kv_store.hpp"
#include <filesystem>
#include <span>
namespace core_os::native
{
// Attach before boot. Directory is exclusively owned by the vdev OS lock.
// Default KvStore remains in-memory; each durable blob commits separately.
Err openFileStore(KvStore &store, const std::filesystem::path &directory);
Err writeFile(const std::filesystem::path &path, std::span<const uint8_t> bytes);
Err eraseFile(const std::filesystem::path &path);
// An unclean process exit is a crash. Configure before starting the core.
Err openBootFile(const std::filesystem::path &path);
Err cleanBootFile();
bool bootFileHealthy();
void resetBootClock();
} // namespace core_os::native
