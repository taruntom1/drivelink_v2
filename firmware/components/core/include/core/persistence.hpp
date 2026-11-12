#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/config_blob.hpp"
#include "core/instance.hpp"
#include "core/status.hpp"
#include "core_os/kv_store.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core::persist {

namespace defs = drivelink::protocol::defs;

// NVS keys. Each instance has a namespace `t<type>i<inst>` (two hex digits
// each; the system is t00i00) with its param blob in `p_a` / `p_b`. The layout
// is in namespace `core`, keys `layout_a` / `layout_b`. Generation `gen` is
// written to slot `gen & 1` (0 = `_a`, 1 = `_b`).
inline constexpr const char *kLayoutNamespace = "core";
inline constexpr const char *kLayoutKeys[2] = {"layout_a", "layout_b"};
inline constexpr const char *kParamKeys[2] = {"p_a", "p_b"};

/// `core.NvsWriteFailed` arg0 for the layout namespace, and arg1 values.
/// Part of the diag contract (protocol/defs/diag_codes.yaml): never renumber.
inline constexpr uint32_t kNvsArgLayout = 0xFFFF;
inline constexpr uint32_t kNvsOpWrite = 1;
inline constexpr uint32_t kNvsOpErase = 2;

/// `core.ConfigLoadFailed` arg1 values (diag contract).
inline constexpr uint32_t kLoadNoLayout = 1;   ///< no layout blob passed its checks
inline constexpr uint32_t kLoadIncomplete = 2; ///< no generation had every instance blob

/// A namespace name: `t%02xi%02x` and its NUL.
struct NsName {
    char text[8];
};

NsName instanceNamespace(uint8_t type_id, uint8_t instance);

/// Parse `t%02xi%02x` (lower-case hex). @return false for any other name.
bool parseInstanceNamespace(const char *ns, uint8_t &type_id, uint8_t &instance);

/// One instance's stored configuration.
struct StoredInstance {
    uint8_t type_id;
    uint8_t instance;
    uint8_t loop;
    char label[kLabelBytes + 1];
    std::vector<ParamPair> pairs;
};

/// What `loadConfig` found.
struct LoadedConfig {
    bool found_any = false;  ///< a layout blob exists (valid or not): not a blank store
    bool complete = false;   ///< a whole generation was loaded
    uint32_t gen = 0;        ///< the generation loaded
    uint32_t newest_gen = 0; ///< the newest layout generation that passed its checks
    uint32_t cause = 0;      ///< when !complete && found_any: kLoadNoLayout or kLoadIncomplete
    std::vector<StoredInstance> instances; ///< in layout order
    std::vector<ParamPair> system;         ///< the system params (t00i00)
};

/// Load the newest complete generation: the valid layout slot with the
/// highest generation, with every instance blob (and the system's) of that
/// same generation; else the other slot's generation; else nothing. Reports
/// nothing. Configure time (it allocates).
LoadedConfig loadConfig(core_os::KvStore &store);

/// Write generation `gen` to slot `gen & 1`: every instance blob, the
/// system's, then the layout blob as the commit record. Then erase the
/// namespace of every instance that is not in `instances` (`t??i??` but
/// t00i00). A failed write reports `core.NvsWriteFailed` and answers `Fault`
/// / `StorageFailed`, and the previous generation stays intact. A failed
/// erase after the commit is reported, and the answer stays Ok.
Status saveConfig(core_os::KvStore &store, uint32_t gen, std::span<const StoredInstance> instances,
                  std::span<const ParamPair> system);

/// Erase the layout namespace and every instance namespace. A failure is
/// reported (`core.NvsWriteFailed`) and answers `Fault` / `StorageFailed`.
Status eraseAll(core_os::KvStore &store);

/// Fill `shadow` (one slot per item of `desc`) from stored pairs: every param
/// starts at its default; a pair whose id is unknown or retired, or names a
/// param that is not `persist`, is ignored; a value that fails `checkValue`
/// keeps the default; one out of range is clamped.
void applyPairs(const defs::TypeDesc &desc, std::span<const ParamPair> pairs, uint32_t *shadow);

/// The pairs SAVE writes for one instance: every `persist` param, its value
/// taken from `value(index)`.
template <typename ValueAt> std::vector<ParamPair> persistPairs(const defs::TypeDesc &desc, ValueAt value)
{
    std::vector<ParamPair> pairs;
    for (size_t i = 0; i < desc.items.size(); ++i) {
        const defs::ItemDesc &item = desc.items[i];
        if (item.kind == defs::ItemKind::Param && defs::hasFlag(item, defs::ItemFlag::Persist)) {
            pairs.push_back(ParamPair{item.id, value(i)});
        }
    }
    return pairs;
}

} // namespace core::persist
