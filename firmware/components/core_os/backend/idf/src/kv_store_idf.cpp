// core_os, ESP-IDF backend: the KvStore on NVS (default partition, handle
// API), nvsInit(), the boot info (reset reason, RTC no-init crash counter,
// reboot) and the one-shot timer (esp_timer).

#include <array>
#include <cstring>
#include <vector>

#include "core_os/boot_info.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/one_shot.hpp"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *const TAG = "core_os_kv";

namespace core_os {

// -- KvStore ---------------------------------------------------------------------

KvStore::KvStore() = default;
KvStore::~KvStore() = default;

Err KvStore::get(const char *ns, const char *key, void *buf, size_t &len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err; // ESP_ERR_NVS_NOT_FOUND for a namespace that was never written
    }
    err = nvs_get_blob(handle, key, buf, &len);
    nvs_close(handle);
    return err;
}

Err KvStore::set(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

Err KvStore::eraseNamespace(const char *ns)
{
    nvs_handle_t handle = 0;
    // Read-only first: opening a missing namespace read-write would create it.
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_close(handle);
    err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

Err KvStore::listNamespaces(NamespaceFn fn, void *ctx)
{
    if (fn == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Collect the distinct names first: the iterator walks entries, not
    // namespaces, and fn may erase what it is walking. Configure time, so the
    // vector may allocate.
    using Name = std::array<char, NVS_NS_NAME_MAX_SIZE>;
    std::vector<Name> names;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info{};
        if (nvs_entry_info(it, &info) == ESP_OK) {
            bool seen = false;
            for (const Name &n : names) {
                if (std::strncmp(n.data(), info.namespace_name, n.size()) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                Name n{};
                std::memcpy(n.data(), info.namespace_name, strnlen(info.namespace_name, n.size() - 1U));
                names.push_back(n);
            }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it); // null after the last entry; releasing null is fine
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    for (const Name &n : names) {
        fn(ctx, n.data());
    }
    return ESP_OK;
}

NvsInitResult nvsInit()
{
    NvsInitResult r{ESP_OK, ESP_OK, false};
    esp_err_t err = nvs_flash_init();
    r.first_err = err;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init: %s; erasing the partition", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            r.erased = true;
            err = nvs_flash_init();
        }
    }
    r.err = err;
    return r;
}

// -- boot info -------------------------------------------------------------------

namespace {

constexpr uint32_t kCrashMagic = 0x43525348U; // "CRSH"

// RTC slow memory, not initialised at boot: survives every reset but a power-on.
RTC_NOINIT_ATTR uint32_t s_crash_magic;
RTC_NOINIT_ATTR uint32_t s_crash_count;
RTC_NOINIT_ATTR uint32_t s_crash_check;

} // namespace

bool resetWasCrash()
{
    const esp_reset_reason_t reason = esp_reset_reason();
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT;
}

uint32_t loadCrashCount()
{
    if (s_crash_magic != kCrashMagic || s_crash_check != ~s_crash_count) {
        return 0U; // power-on: the memory holds garbage
    }
    return s_crash_count;
}

void storeCrashCount(uint32_t count)
{
    s_crash_count = count;
    s_crash_check = ~count;
    s_crash_magic = kCrashMagic;
}

void reboot()
{
    esp_restart();
}

// -- OneShotTimer ----------------------------------------------------------------

OneShotTimer::OneShotTimer() = default;

OneShotTimer::~OneShotTimer()
{
    cancel();
    if (m_state.handle != nullptr) {
        (void)esp_timer_delete(m_state.handle);
        m_state.handle = nullptr;
    }
}

Err OneShotTimer::start(const char *name, uint32_t delay_us, OneShotFn fn, void *ctx)
{
    if (fn == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    cancel();
    m_state.fn = fn;
    m_state.ctx = ctx;
    if (m_state.handle == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = [](void *arg) {
                auto *state = static_cast<detail::OneShotState *>(arg);
                state->fn(state->ctx);
            },
            .arg = &m_state,
            .dispatch_method = ESP_TIMER_TASK,
            .name = name,
            .skip_unhandled_events = true,
        };
        const esp_err_t err = esp_timer_create(&args, &m_state.handle);
        if (err != ESP_OK) {
            m_state.handle = nullptr;
            return err;
        }
    }
    return esp_timer_start_once(m_state.handle, delay_us);
}

void OneShotTimer::cancel()
{
    if (m_state.handle != nullptr) {
        // ESP_ERR_INVALID_STATE when it is not running: nothing to cancel.
        (void)esp_timer_stop(m_state.handle);
    }
}

} // namespace core_os
