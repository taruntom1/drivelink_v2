#pragma once

// Native backend only: fault injection for the in-memory KvStore, for the
// tests. A "write" is a `set` or an `eraseNamespace` call; the counts below
// are writes from now on.

#include <cstddef>
#include <cstdint>
#include <string>

#include "core_os/kv_store.hpp"

namespace core_os::native {

/// How a torn write damages the blob it stores.
enum class Tear : uint8_t {
    Truncate, ///< only the first half of the bytes reach the store
    Corrupt,  ///< every byte is stored, but the last one is flipped
};

/// Power loss: the next `n` writes succeed, then every write fails (returns
/// an error and changes nothing) until `clearFaults`.
void failWritesAfter(KvStore &store, uint32_t n);

/// The write after the next `n` fails once with `err` and changes nothing;
/// the writes after it succeed.
void failWriteAfter(KvStore &store, uint32_t n, Err err);
inline void failNextWrite(KvStore &store, Err err)
{
    failWriteAfter(store, 0, err);
}

/// The write after the next `n`, if it is a `set`, stores a damaged blob and
/// still reports success (as if the power went right after it).
void tearWriteAfter(KvStore &store, uint32_t n, Tear mode = Tear::Truncate);
inline void tearNextWrite(KvStore &store, Tear mode = Tear::Truncate)
{
    tearWriteAfter(store, 0, mode);
}

/// Drop every pending fault.
void clearFaults(KvStore &store);

/// Writes that took effect since the store was made.
uint32_t writeCount(KvStore &store);

/// True if `ns` holds `key`.
bool hasKey(KvStore &store, const std::string &ns, const std::string &key);

/// Remove one key (a blob lost behind the core's back).
void removeKey(KvStore &store, const std::string &ns, const std::string &key);

/// How many namespaces hold at least one key.
size_t namespaceCount(KvStore &store);

} // namespace core_os::native
