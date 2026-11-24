#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/instance.hpp"
#include "core/instance_table.hpp"
#include "core/layout_validation.hpp"
#include "core/param_service.hpp"
#include "core/registry.hpp"
#include "core/status.hpp"

namespace core {

/// One entry of the pending layout.
struct PendingEntry {
    const defs::TypeDesc *desc;
    uint8_t type_id;
    uint8_t instance;
    uint8_t loop;
    char label[kLabelBytes + 1];
    /// The configured record it was copied from, or nullptr for an instance
    /// added to the pending layout.
    detail::InstanceRecord *origin;
    /// Value bits by item index. For an added instance every param lives
    /// here. For a copied one only the `reboot_required` params do: its live
    /// params stay in the record's shadow (`valueAt()`).
    std::vector<uint32_t> values;
};

/// The pending copy: the layout and the `reboot_required` system settings the
/// device will run after SAVE and a reboot.
/// Config task side only (one caller at a time); its ops are configure
/// operations and may allocate.
///
/// It is created at the first pending operation by copying every configured
/// record and the saved system settings, and lives until `discard()` or the
/// reboot. The system is no layout entry. The whole layout is validated on
/// every op (`validateLayout`), against the limits that the pending system
/// settings give, and an op is
/// undone and refused when, after it, an entry fails that passed before, or an
/// entry the op added or changed fails. The refusal carries that entry's
/// status.
class PendingLayout {
public:
    PendingLayout(const Registry &registry, detail::InstanceTable &table, const CoreConfig &config);

    PendingLayout(const PendingLayout &) = delete;
    PendingLayout &operator=(const PendingLayout &) = delete;

    /// The saved system values, by item index (set by the boot). The pending
    /// copy of the `reboot_required` ones starts from them. In safe mode they
    /// differ from the running values.
    void setSavedSystem(std::span<const uint32_t> values);

    /// The value of system item `index` that SAVE writes: the pending (else
    /// the saved) one of a `reboot_required` param, the live one of any other.
    uint32_t systemValue(size_t index) const;

    /// True once an op succeeded (until `discard()`): the pending layout
    /// differs from the configured one.
    bool active() const { return m_active; }

    /// The entries, in layout order (copied records first, then the added
    /// ones). Empty while inactive.
    std::span<const PendingEntry> entries() const { return m_entries; }

    /// Add an instance of `type_id` in `loop`. The device picks the lowest
    /// instance id the type has free in the pending layout (0..254), and
    /// returns it in `instance`. Every param starts at its default.
    Status addInstance(uint8_t type_id, uint8_t loop, const char *label, uint8_t &instance);

    /// Remove (type, inst). Refused while another entry is wired to it
    /// (`InvalidArgument` / `Referenced`).
    Status removeInstance(uint8_t type_id, uint8_t instance);

    /// Change the label of (type, inst).
    Status setLabel(uint8_t type_id, uint8_t instance, const char *label);

    /// Drop the pending layout. Idempotent.
    void discard();

    /// Write a pending param batch. The param service has checked each entry
    /// (an existing param, writable, its value checked and clamped into
    /// `applied`); this checks that each (type, inst) is the system or in the
    /// pending layout (`UnknownRef`), writes the values and validates. A
    /// refusal names batch entry 0 when the batch wrote nothing to the layout
    /// entry that failed (a system setting lowered its limit).
    Status setValues(std::span<const SetEntry> entries, std::span<const uint32_t> applied);

    /// The entry of an instance that exists only in the pending layout (an
    /// added one), or nullptr.
    const PendingEntry *findAdded(uint8_t type_id, uint8_t instance) const;

    /// Read params as the pending layout has them (GET_PENDING): an added
    /// instance's params and every `reboot_required` param from the pending
    /// values, a copied instance's live params from its record, the system's
    /// as `systemValue()`. Inactive, it reads the configured records. `UnknownRef`, `WrongKind`,
    /// `BufferTooSmall` as for `ParamService::get`.
    Status get(std::span<const Ref> refs, std::span<uint32_t> values) const;

    /// The value SAVE writes for item `index` of `entry`.
    static uint32_t valueAt(const PendingEntry &entry, size_t index);

private:
    bool ensureCopy();
    PendingEntry *find(uint8_t type_id, uint8_t instance);
    std::vector<LayoutEntry> view() const;
    /// The layout's limits with these system values.
    ValidationConfig limits(const std::vector<uint32_t> &system) const;
    /// Validate after an op. `before` and `before_system` are the copy before
    /// it; `touched` names the (type, inst) the op added or changed.
    Status check(const std::vector<PendingEntry> &before, const std::vector<uint32_t> &before_system,
                 std::span<const uint16_t> touched) const;
    /// Run `op` with the snapshot / validate / undo rule.
    template <typename Op> Status run(Op op);

    const Registry &m_registry;
    detail::InstanceTable &m_table;
    const CoreConfig m_config;
    bool m_active = false;
    std::vector<PendingEntry> m_entries;
    std::vector<uint32_t> m_saved_system; ///< by item index, as loaded at boot
    std::vector<uint32_t> m_system;       ///< by item index, empty while inactive
};

} // namespace core
