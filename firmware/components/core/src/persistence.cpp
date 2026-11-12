// NVS A/B persistence (core/persistence.hpp): pure apart from the KvStore and
// the diag sink. Configure time: it allocates its buffers.

#include "core/persistence.hpp"

#include <cstring>

#include "core/value.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/registry.hpp"

namespace core::persist {

namespace {

namespace diag_codes = drivelink::protocol::defs::diag::codes;

/// A stored blob larger than this is invalid (the largest real one, a layout
/// of 255 entries, is under 5 KB).
constexpr size_t kMaxBlobBytes = 8192;

constexpr char kHex[] = "0123456789abcdef";

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/// Read a whole blob into `out`. @return false when it is missing, too big or
/// unreadable.
bool readBlob(core_os::KvStore &store, const char *ns, const char *key, std::vector<uint8_t> &out)
{
    size_t len = 0;
    if (store.get(ns, key, nullptr, len) != core_os::kOk || len == 0U || len > kMaxBlobBytes) {
        return false;
    }
    out.resize(len);
    if (store.get(ns, key, out.data(), len) != core_os::kOk || len != out.size()) {
        return false;
    }
    return true;
}

uint32_t nsArg(uint8_t type_id, uint8_t instance)
{
    return (static_cast<uint32_t>(type_id) << 8U) | instance;
}

/// Read the instance blob of (type, inst) at generation `gen`.
bool loadInstance(core_os::KvStore &store, uint8_t type_id, uint8_t instance, uint32_t gen,
                  std::vector<uint8_t> &buf, std::vector<ParamPair> &pairs)
{
    const NsName ns = instanceNamespace(type_id, instance);
    uint32_t blob_gen = 0;
    return readBlob(store, ns.text, kParamKeys[gen & 1U], buf) && decodeInstanceBlob(buf, blob_gen, pairs) &&
           blob_gen == gen;
}

struct EraseContext {
    core_os::KvStore *store;
    std::span<const StoredInstance> keep; ///< empty: erase every instance namespace
    bool failed;
};

void eraseDropped(void *ctx, const char *ns)
{
    auto *c = static_cast<EraseContext *>(ctx);
    uint8_t type_id = 0;
    uint8_t instance = 0;
    if (!parseInstanceNamespace(ns, type_id, instance)) {
        return; // not ours
    }
    if (type_id == drivelink::protocol::defs::kSystemTypeId && !c->keep.empty()) {
        return; // the system's params stay (SAVE always writes them)
    }
    for (const StoredInstance &s : c->keep) {
        if (s.type_id == type_id && s.instance == instance) {
            return;
        }
    }
    const core_os::Err err = c->store->eraseNamespace(ns);
    if (err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kNvsWriteFailed, err, nsArg(type_id, instance), kNvsOpErase);
        c->failed = true;
    }
}

} // namespace

NsName instanceNamespace(uint8_t type_id, uint8_t instance)
{
    NsName n{};
    n.text[0] = 't';
    n.text[1] = kHex[type_id >> 4U];
    n.text[2] = kHex[type_id & 0xFU];
    n.text[3] = 'i';
    n.text[4] = kHex[instance >> 4U];
    n.text[5] = kHex[instance & 0xFU];
    n.text[6] = '\0';
    return n;
}

bool parseInstanceNamespace(const char *ns, uint8_t &type_id, uint8_t &instance)
{
    if (ns == nullptr || std::strlen(ns) != 6U || ns[0] != 't' || ns[3] != 'i') {
        return false;
    }
    const int d[4] = {hexValue(ns[1]), hexValue(ns[2]), hexValue(ns[4]), hexValue(ns[5])};
    for (const int v : d) {
        if (v < 0) {
            return false;
        }
    }
    type_id = static_cast<uint8_t>((d[0] << 4) | d[1]);
    instance = static_cast<uint8_t>((d[2] << 4) | d[3]);
    return true;
}

LoadedConfig loadConfig(core_os::KvStore &store)
{
    LoadedConfig cfg;
    std::vector<uint8_t> buf;

    struct Slot {
        bool valid = false;
        uint32_t gen = 0;
        std::vector<LayoutRecord> records;
    };
    Slot slots[2];
    for (size_t s = 0; s < 2U; ++s) {
        size_t len = 0;
        if (store.get(kLayoutNamespace, kLayoutKeys[s], nullptr, len) == core_os::kOk) {
            cfg.found_any = true;
        }
        if (readBlob(store, kLayoutNamespace, kLayoutKeys[s], buf)) {
            slots[s].valid = decodeLayoutBlob(buf, slots[s].gen, slots[s].records);
            // A blob in the wrong slot for its generation is not trusted.
            slots[s].valid = slots[s].valid && ((slots[s].gen & 1U) == s);
        }
        if (slots[s].valid && slots[s].gen > cfg.newest_gen) {
            cfg.newest_gen = slots[s].gen;
        }
    }
    if (!slots[0].valid && !slots[1].valid) {
        cfg.cause = kLoadNoLayout;
        return cfg;
    }

    // Newest generation first.
    size_t order[2] = {0, 1};
    if (!slots[0].valid || (slots[1].valid && slots[1].gen > slots[0].gen)) {
        order[0] = 1;
        order[1] = 0;
    }
    for (const size_t s : order) {
        const Slot &slot = slots[s];
        if (!slot.valid) {
            continue;
        }
        std::vector<StoredInstance> instances;
        instances.reserve(slot.records.size());
        bool complete = true;
        for (const LayoutRecord &rec : slot.records) {
            StoredInstance inst{};
            inst.type_id = rec.type_id;
            inst.instance = rec.instance;
            inst.loop = rec.loop;
            std::memcpy(inst.label, rec.label, sizeof(inst.label));
            if (!loadInstance(store, rec.type_id, rec.instance, slot.gen, buf, inst.pairs)) {
                complete = false;
                break;
            }
            instances.push_back(std::move(inst));
        }
        std::vector<ParamPair> system;
        if (complete &&
            !loadInstance(store, drivelink::protocol::defs::kSystemTypeId, 0, slot.gen, buf, system)) {
            complete = false;
        }
        if (complete) {
            cfg.complete = true;
            cfg.gen = slot.gen;
            cfg.cause = 0;
            cfg.instances = std::move(instances);
            cfg.system = std::move(system);
            return cfg;
        }
    }
    cfg.cause = kLoadIncomplete;
    return cfg;
}

Status saveConfig(core_os::KvStore &store, uint32_t gen, std::span<const StoredInstance> instances,
                  std::span<const ParamPair> system)
{
    const size_t slot = gen & 1U;
    auto writeInstance = [&](uint8_t type_id, uint8_t instance, std::span<const ParamPair> pairs) {
        const std::vector<uint8_t> blob = encodeInstanceBlob(gen, pairs);
        const NsName ns = instanceNamespace(type_id, instance);
        const core_os::Err err =
            blob.empty() ? core_os::Err{-1} : store.set(ns.text, kParamKeys[slot], blob.data(), blob.size());
        if (err != core_os::kOk) {
            core_os::reportDiag(diag_codes::core::kNvsWriteFailed, err, nsArg(type_id, instance), kNvsOpWrite);
            return false;
        }
        return true;
    };

    // 1. Every instance blob, and the system's.
    for (const StoredInstance &s : instances) {
        if (!writeInstance(s.type_id, s.instance, s.pairs)) {
            return Status::fail(Result::Fault, Detail::StorageFailed);
        }
    }
    if (!writeInstance(drivelink::protocol::defs::kSystemTypeId, 0, system)) {
        return Status::fail(Result::Fault, Detail::StorageFailed);
    }

    // 2. The layout blob: the commit record.
    std::vector<LayoutRecord> records;
    records.reserve(instances.size());
    for (const StoredInstance &s : instances) {
        LayoutRecord rec{};
        rec.type_id = s.type_id;
        rec.instance = s.instance;
        rec.loop = s.loop;
        std::memcpy(rec.label, s.label, sizeof(rec.label));
        records.push_back(rec);
    }
    const std::vector<uint8_t> layout = encodeLayoutBlob(gen, records);
    const core_os::Err err = layout.empty() ? core_os::Err{-1}
                                            : store.set(kLayoutNamespace, kLayoutKeys[slot], layout.data(),
                                                        layout.size());
    if (err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kNvsWriteFailed, err, kNvsArgLayout, kNvsOpWrite);
        return Status::fail(Result::Fault, Detail::StorageFailed);
    }

    // 3. Drop the namespaces of instances that are gone. Committed already, so
    //    a failure here is only reported.
    EraseContext ctx{&store, instances, false};
    if (instances.empty()) {
        // An empty layout: keep nothing but the system. eraseDropped treats an
        // empty `keep` as "erase everything", so give it the system to keep.
        static const StoredInstance kSystemOnly{drivelink::protocol::defs::kSystemTypeId, 0, 0, {}, {}};
        ctx.keep = {&kSystemOnly, 1};
    }
    (void)store.listNamespaces(&eraseDropped, &ctx);
    return Status::success();
}

Status eraseAll(core_os::KvStore &store)
{
    bool failed = false;
    const core_os::Err err = store.eraseNamespace(kLayoutNamespace);
    if (err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kNvsWriteFailed, err, kNvsArgLayout, kNvsOpErase);
        failed = true;
    }
    EraseContext ctx{&store, {}, false};
    const core_os::Err list_err = store.listNamespaces(&eraseDropped, &ctx);
    if (list_err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kNvsWriteFailed, list_err, kNvsArgLayout, kNvsOpErase);
        failed = true;
    }
    if (failed || ctx.failed) {
        return Status::fail(Result::Fault, Detail::StorageFailed);
    }
    return Status::success();
}

void applyPairs(const defs::TypeDesc &desc, std::span<const ParamPair> pairs, uint32_t *shadow)
{
    for (size_t i = 0; i < desc.items.size(); ++i) {
        const defs::ItemDesc &item = desc.items[i];
        shadow[i] = (item.kind == defs::ItemKind::Param) ? item.default_bits : 0U;
    }
    for (const ParamPair &p : pairs) {
        for (size_t i = 0; i < desc.items.size(); ++i) {
            const defs::ItemDesc &item = desc.items[i];
            if (item.id != p.id) {
                continue;
            }
            if (item.kind == defs::ItemKind::Param && defs::hasFlag(item, defs::ItemFlag::Persist)) {
                const ValueCheck check = checkValue(item, p.value);
                if (check.ok) {
                    shadow[i] = check.applied_bits;
                }
            }
            break;
        }
    }
}

} // namespace core::persist
