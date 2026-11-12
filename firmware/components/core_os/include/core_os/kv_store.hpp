#pragma once

#include <cstddef>
#include <cstdint>

#include "core_os/detail/backend_types.hpp"
#include "core_os/types.hpp"

namespace core_os {

/// The longest namespace or key name, in characters (the NVS limit).
inline constexpr size_t kKvNameMax = 15;

/// Called once per namespace by `KvStore::listNamespaces`.
using NamespaceFn = void (*)(void *ctx, const char *ns);

/// A blob store of namespaces and keys: NVS on the device (the default
/// partition, through the `nvs` handle API), an in-memory map natively (with
/// fault injection, `core_os/native_kv.hpp`).
///
/// Names are at most `kKvNameMax` characters. Every call is a configure-time
/// operation: it may block on flash and must never run on a real-time path
/// (a flash write stalls both cores' caches). Not thread-safe: one caller (the
/// config task) at a time.
class KvStore {
public:
    KvStore();
    ~KvStore();

    KvStore(const KvStore &) = delete;
    KvStore &operator=(const KvStore &) = delete;

    /// Read blob `key` of namespace `ns`. With `buf` null, `len` receives the
    /// stored size. Otherwise `len` is the capacity of `buf` on entry and the
    /// blob's size on return; a blob larger than `len` is an error and
    /// nothing is copied. @return kOk, or an error (missing namespace or
    /// key, too small, a flash error).
    Err get(const char *ns, const char *key, void *buf, size_t &len);

    /// Write blob `key` of namespace `ns` (creating both as needed) and
    /// commit it. On an error the previous value may be gone, but no other
    /// key is touched.
    Err set(const char *ns, const char *key, const void *data, size_t len);

    /// Erase every key of `ns` and commit. A missing namespace is kOk.
    Err eraseNamespace(const char *ns);

    /// Call `fn(ctx, ns)` once for each namespace that holds at least one
    /// key. The list is taken first, so `fn` may erase namespaces.
    Err listNamespaces(NamespaceFn fn, void *ctx);

    /// The backend's state, for the backend's own extras (the native fault
    /// injection). Not for the core.
    detail::KvStoreState &backendState() { return m_state; }

private:
    detail::KvStoreState m_state;
};

/// What `nvsInit()` did.
struct NvsInitResult {
    Err err;        ///< kOk when the store is usable
    Err first_err;  ///< what the first init returned (the reason for an erase)
    bool erased;    ///< the partition was erased and re-initialised: the stored data is gone
};

/// Initialise the store once at boot, before any `KvStore` call. On the
/// device: `nvs_flash_init()`; if the partition has no free pages or was
/// written by a newer NVS version, it is erased and initialised again (and
/// `erased` says so). Natively: nothing to do.
NvsInitResult nvsInit();

} // namespace core_os
