// core_os, native backend: the in-memory KvStore with fault injection, the
// boot info fakes and the one-shot timer (a thread). Built by
// firmware/test/native (and later the vdev, which adds a file-backed store).

#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#include "core_os/boot_info.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_kv.hpp"
#include "core_os/one_shot.hpp"
#include "core_os/native_file.hpp"
#include <filesystem>
#include "core_os/realtime.hpp"

namespace core_os {
namespace native { void persistCrashCount(uint32_t count); }

namespace {

constexpr Err kNativeError = -1;
constexpr Err kNotFound = -2;
constexpr Err kTooSmall = -3;

bool validName(const char *name)
{
    return name != nullptr && name[0] != '\0' && std::strlen(name) <= kKvNameMax;
}

/// What happens to the write being made, after counting it down.
enum class WriteFate : uint8_t {
    Normal,
    Fail,
    Tear,
};

WriteFate nextWrite(detail::KvStoreState &s, Err &err)
{
    err = kNativeError;
    if (s.power_loss_after == 0) {
        return WriteFate::Fail; // stays 0: every write fails
    }
    if (s.power_loss_after > 0) {
        --s.power_loss_after;
    }
    WriteFate fate = WriteFate::Normal;
    if (s.fail_after == 0) {
        fate = WriteFate::Fail;
        err = s.fail_err;
    }
    if (s.fail_after >= 0) {
        --s.fail_after;
    }
    if (s.tear_after == 0 && fate == WriteFate::Normal) {
        fate = WriteFate::Tear;
    }
    if (s.tear_after >= 0) {
        --s.tear_after;
    }
    return fate;
}

} // namespace

KvStore::KvStore() = default;
KvStore::~KvStore() = default;

Err KvStore::get(const char *ns, const char *key, void *buf, size_t &len)
{
    recordRealtimeViolation();
    if (!validName(ns) || !validName(key)) {
        return kNativeError;
    }
    const auto nit = m_state.data.find(ns);
    if (nit == m_state.data.end()) {
        return kNotFound;
    }
    const auto kit = nit->second.find(key);
    if (kit == nit->second.end()) {
        return kNotFound;
    }
    const std::vector<uint8_t> &blob = kit->second;
    if (buf == nullptr) {
        len = blob.size();
        return kOk;
    }
    if (len < blob.size()) {
        return kTooSmall;
    }
    std::memcpy(buf, blob.data(), blob.size());
    len = blob.size();
    return kOk;
}

Err KvStore::set(const char *ns, const char *key, const void *data, size_t len)
{
    recordRealtimeViolation();
    if (!validName(ns) || !validName(key) || (data == nullptr && len > 0U)) {
        return kNativeError;
    }
    Err err = kOk;
    const WriteFate fate = nextWrite(m_state, err);
    if (fate == WriteFate::Fail) {
        return err;
    }
    const auto *bytes = static_cast<const uint8_t *>(data);
    std::vector<uint8_t> blob(bytes, bytes + len);
    if (fate == WriteFate::Tear) {
        if (static_cast<native::Tear>(m_state.tear_mode) == native::Tear::Truncate) {
            blob.resize(len / 2U);
        } else if (!blob.empty()) {
            blob.back() = static_cast<uint8_t>(blob.back() ^ 0xFFU);
        }
    }
    if (!m_state.directory.empty()) {
        const auto safe=[](const char *s) { return std::string(s).find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")==std::string::npos; };
        if(!safe(ns) || !safe(key) || len>65536) { return kNativeError; }
        if(native::writeFile(std::filesystem::path(m_state.directory)/(std::string(ns)+"."+key+".blob"),blob)!=kOk) { return kNativeError; }
    }
    m_state.data[ns][key] = std::move(blob);
    ++m_state.write_count;
    return kOk;
}

Err KvStore::eraseNamespace(const char *ns)
{
    recordRealtimeViolation();
    if (!validName(ns)) {
        return kNativeError;
    }
    Err err = kOk;
    if (nextWrite(m_state, err) == WriteFate::Fail) {
        return err;
    }
    if(!m_state.directory.empty()) {
        const auto it=m_state.data.find(ns);
        if(it!=m_state.data.end()) {
            for(const auto &entry:it->second) {
                if(native::eraseFile(std::filesystem::path(m_state.directory)/(std::string(ns)+"."+entry.first+".blob"))!=kOk) { return kNativeError; }
            }
        }
    }
    m_state.data.erase(ns);
    ++m_state.write_count;
    return kOk;
}

Err KvStore::listNamespaces(NamespaceFn fn, void *ctx)
{
    recordRealtimeViolation();
    if (fn == nullptr) {
        return kNativeError;
    }
    std::vector<std::string> names;
    for (const auto &entry : m_state.data) {
        if (!entry.second.empty()) {
            names.push_back(entry.first);
        }
    }
    for (const std::string &name : names) {
        fn(ctx, name.c_str());
    }
    return kOk;
}

NvsInitResult nvsInit()
{
    return NvsInitResult{kOk, kOk, false};
}

namespace native {

void failWritesAfter(KvStore &store, uint32_t n)
{
    store.backendState().power_loss_after = n;
}

void failWriteAfter(KvStore &store, uint32_t n, Err err)
{
    store.backendState().fail_after = n;
    store.backendState().fail_err = err;
}

void tearWriteAfter(KvStore &store, uint32_t n, Tear mode)
{
    store.backendState().tear_after = n;
    store.backendState().tear_mode = static_cast<uint8_t>(mode);
}

void clearFaults(KvStore &store)
{
    detail::KvStoreState &s = store.backendState();
    s.power_loss_after = -1;
    s.fail_after = -1;
    s.tear_after = -1;
}

uint32_t writeCount(KvStore &store)
{
    return store.backendState().write_count;
}

bool hasKey(KvStore &store, const std::string &ns, const std::string &key)
{
    const auto &data = store.backendState().data;
    const auto nit = data.find(ns);
    return nit != data.end() && nit->second.count(key) != 0U;
}

void removeKey(KvStore &store, const std::string &ns, const std::string &key)
{
    auto &data = store.backendState().data;
    const auto nit = data.find(ns);
    if (nit != data.end()) {
        nit->second.erase(key);
        if (nit->second.empty()) {
            data.erase(nit);
        }
    }
}

size_t namespaceCount(KvStore &store)
{
    size_t n = 0;
    for (const auto &entry : store.backendState().data) {
        n += entry.second.empty() ? 0U : 1U;
    }
    return n;
}

} // namespace native

// -- boot info -------------------------------------------------------------------

namespace {

std::atomic<bool> s_reset_was_crash{false};
std::atomic<uint32_t> s_crash_count{0};
std::atomic<uint32_t> s_reboots{0};

} // namespace

bool resetWasCrash()
{
    return s_reset_was_crash.load();
}

uint32_t loadCrashCount()
{
    return s_crash_count.load();
}

void storeCrashCount(uint32_t count)
{
    s_crash_count.store(count);
    native::persistCrashCount(count);
}

void reboot()
{
    s_reboots.fetch_add(1U);
}

namespace native {

void loadCrashCountForFile(uint32_t count) { s_crash_count.store(count); }

void setResetWasCrash(bool crash)
{
    s_reset_was_crash.store(crash);
}

uint32_t rebootCount()
{
    return s_reboots.load();
}

void resetBootInfo()
{
    s_reset_was_crash.store(false);
    s_crash_count.store(0U);
    s_reboots.store(0U);
}

} // namespace native

// -- OneShotTimer ----------------------------------------------------------------

OneShotTimer::OneShotTimer() = default;

OneShotTimer::~OneShotTimer()
{
    cancel();
}

Err OneShotTimer::start(const char *name, uint32_t delay_us, OneShotFn fn, void *ctx)
{
    (void)name;
    if (fn == nullptr) {
        return kNativeError;
    }
    cancel();
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.cancel = false;
    }
    detail::OneShotState *state = &m_state;
    m_state.thread = std::thread([state, delay_us, fn, ctx]() {
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool cancelled = state->cv.wait_for(lock, std::chrono::microseconds(delay_us),
                                                  [state] { return state->cancel; });
        if (!cancelled) {
            lock.unlock();
            fn(ctx);
        }
    });
    return kOk;
}

void OneShotTimer::cancel()
{
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.cancel = true;
    }
    m_state.cv.notify_one();
    if (m_state.thread.joinable()) {
        m_state.thread.join();
    }
}

} // namespace core_os
