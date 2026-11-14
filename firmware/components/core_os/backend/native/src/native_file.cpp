#include "core_os/native_file.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/native_boot.hpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace core_os::native
{
namespace fs = std::filesystem;
namespace
{
bool valid(const std::string &s)
{
    return !s.empty() && s.size() <= kKvNameMax &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
               std::string::npos;
}
Err syncDirectory(const fs::path &path)
{
#ifdef _WIN32
    (void)path;
    return kOk; // MoveFileExW(WRITE_THROUGH) below
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        return -1;
    }
    const int result = fsync(fd);
    ::close(fd);
    return result == 0 ? kOk : -1;
#endif
}
} // namespace
Err writeFile(const fs::path &path, std::span<const uint8_t> bytes)
{
    const auto temporary = fs::path(path.native() + fs::path(".tmp").native());
#ifdef _WIN32
    FILE *file = _wfopen(temporary.c_str(), L"wb");
#else
    FILE *file = std::fopen(temporary.c_str(), "wb");
#endif
    if (!file)
    {
        return -1;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (std::fflush(file) != 0)
    {
        ok = false;
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0)
    {
        ok = false;
    }
#else
    if (fsync(fileno(file)) != 0)
    {
        ok = false;
    }
#endif
    if (std::fclose(file) != 0)
    {
        ok = false;
    }
    if (!ok)
    {
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return -1;
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0)
    {
        return -1;
    }
#endif
    return syncDirectory(path.parent_path());
}
Err eraseFile(const fs::path &path)
{
    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
    {
        return -1;
    }
    return syncDirectory(path.parent_path());
}
Err openFileStore(KvStore &store, const fs::path &directory)
{
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
    {
        return -1;
    }
    decltype(detail::KvStoreState::data) data;
    fs::directory_iterator it(directory, ec);
    if (ec)
    {
        return -1;
    }
    for (const auto &entry : it)
    {
        const auto name = entry.path().filename().string();
        if (!name.ends_with(".blob"))
        {
            continue;
        }
        const auto dot = name.find('.');
        const auto ns = name.substr(0, dot), key = name.substr(dot + 1, name.size() - dot - 6);
        if (!valid(ns) || !valid(key) || !entry.is_regular_file(ec) || entry.is_symlink(ec))
        {
            return -1;
        }
        const auto size = entry.file_size(ec);
        if (ec || size > 65536)
        {
            return -1;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<uint8_t> blob(size);
        if (!file.read(reinterpret_cast<char *>(blob.data()), static_cast<std::streamsize>(size)))
        {
            return -1;
        }
        data[ns][key] = std::move(blob);
    }
    if (data.size() > 254)
    {
        return -1;
    }
    store.backendState().data = std::move(data);
    store.backendState().directory = directory.string();
    return kOk;
}
namespace
{
std::mutex boot_mutex;
fs::path boot_path;
bool boot_healthy = true, active = true;
Err persistBoot(uint32_t count)
{
    if (boot_path.empty())
    {
        return kOk;
    }
    std::array<uint8_t, 10> bytes{'D', 'L', 'B', 1, static_cast<uint8_t>(active), 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < 4; ++i)
    {
        bytes[5 + i] = static_cast<uint8_t>(count >> (i * 8));
    }
    for (unsigned i = 0; i < 9; ++i)
    {
        bytes[9] ^= bytes[i];
    }
    const auto err = writeFile(boot_path, bytes);
    if (err != kOk)
    {
        boot_healthy = false;
    }
    return err;
}
} // namespace
void persistCrashCount(uint32_t count)
{
    std::lock_guard lock(boot_mutex);
    persistBoot(count);
}
Err openBootFile(const fs::path &path)
{
    std::lock_guard lock(boot_mutex);
    std::array<uint8_t, 10> bytes{};
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec)
    {
        return -1;
    }
    uint32_t count = 0;
    bool crashed = false;
    if (exists)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) || file.peek() != EOF)
        {
            return -1;
        }
        uint8_t sum = 0;
        for (auto b : bytes)
        {
            sum ^= b;
        }
        if (sum || bytes[0] != 'D' || bytes[1] != 'L' || bytes[2] != 'B' || bytes[3] != 1 || bytes[4] > 1)
        {
            return -1;
        }
        crashed = bytes[4] != 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            count |= static_cast<uint32_t>(bytes[5 + i]) << (8 * i);
        }
    }
    // Update the atomic fakes without calling the persisting API recursively.
    setResetWasCrash(crashed);
    boot_path = path;
    boot_healthy = true;
    active = true;
    // Load directly to avoid storeCrashCount's persistence hook re-locking us.
    extern void loadCrashCountForFile(uint32_t);
    loadCrashCountForFile(count);
    return persistBoot(count);
}
Err cleanBootFile()
{
    std::lock_guard lock(boot_mutex);
    active = false;
    return persistBoot(loadCrashCount());
}
bool bootFileHealthy()
{
    std::lock_guard lock(boot_mutex);
    return boot_healthy;
}
} // namespace core_os::native
