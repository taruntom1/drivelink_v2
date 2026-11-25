#include "core_os/ota.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include <algorithm>
#include <cstring>

#ifndef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "DriveLink OTA requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE"
#endif

namespace core_os {
struct Ota::Impl {
    const esp_partition_t *target = nullptr;
    esp_ota_handle_t handle = 0;
    bool open = false, finished = false;
};
Ota::Ota() : m_impl(std::make_unique<Impl>()) {}
Ota::~Ota() {
    if (m_impl->open) {
        const auto err=esp_ota_abort(m_impl->handle);
        if (err!=ESP_OK) {
            reportDiag(drivelink::protocol::defs::diag::codes::core::kOtaFailed,err,
                       static_cast<uint32_t>(drivelink::protocol::defs::OtaStage::Abort),0);
        }
    }
}
bool otaImageUnconfirmed() {
    const auto *running=esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running,&state)!=ESP_OK) { return true; }
    return state!=ESP_OTA_IMG_VALID && state!=ESP_OTA_IMG_UNDEFINED;
}
Err Ota::info(OtaBootInfo &out) {
    const auto *running=esp_ota_get_running_partition();
    const auto *selected=esp_ota_get_boot_partition();
    const auto *next=esp_ota_get_next_update_partition(nullptr);
    if (!running || !selected || !next || running->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
        running->subtype>ESP_PARTITION_SUBTYPE_APP_OTA_1 ||
        selected->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_0 || selected->subtype>ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_img_states_t state;
    const auto err=esp_ota_get_state_partition(running,&state);
    if (err!=ESP_OK) { return err; }
    if (state!=ESP_OTA_IMG_VALID && state!=ESP_OTA_IMG_PENDING_VERIFY && state!=ESP_OTA_IMG_UNDEFINED) {
        return ESP_ERR_INVALID_STATE;
    }
    out.running=running->subtype-ESP_PARTITION_SUBTYPE_APP_OTA_0;
    out.selected=selected->subtype-ESP_PARTITION_SUBTYPE_APP_OTA_0;
    out.pending=state==ESP_OTA_IMG_PENDING_VERIFY;
    out.capacity=next->size;
    const auto *desc=esp_app_get_description();
    std::copy_n(desc->app_elf_sha256,32,out.elf.data());
    std::copy_n(desc->version,32,out.version.data());out.version[32]=0;
    return kOk;
}
Err Ota::begin(uint32_t size) {
    auto &d=*m_impl;
    if (d.open) { return ESP_ERR_INVALID_STATE; }
    d.target=esp_ota_get_next_update_partition(nullptr);d.finished=false;
    if (!d.target || !size || size>d.target->size) { return ESP_ERR_INVALID_SIZE; }
    const auto err=esp_ota_begin(d.target,OTA_WITH_SEQUENTIAL_WRITES,&d.handle);
    d.open=err==ESP_OK;return err;
}
Err Ota::write(std::span<const uint8_t> bytes) {
    return m_impl->open ? esp_ota_write(m_impl->handle,bytes.data(),bytes.size()) : ESP_ERR_INVALID_STATE;
}
Err Ota::read(uint32_t offset,std::span<uint8_t> bytes) {
    return m_impl->target ? esp_partition_read(m_impl->target,offset,bytes.data(),bytes.size()) : ESP_ERR_INVALID_STATE;
}
Err Ota::finish() {
    auto &d=*m_impl;if (!d.open) { return ESP_ERR_INVALID_STATE; }
    const auto err=esp_ota_end(d.handle);d.open=false;d.finished=err==ESP_OK;return err;
}
Err Ota::select() {
    return m_impl->finished ? esp_ota_set_boot_partition(m_impl->target) : ESP_ERR_INVALID_STATE;
}
Err Ota::abort() {
    auto &d=*m_impl;
    if (d.open) {
        const auto err=esp_ota_abort(d.handle);if (err!=ESP_OK) { return err; }d.open=false;
    }
    const auto *running=esp_ota_get_running_partition();
    const auto *selected=esp_ota_get_boot_partition();
    if (!running || !selected) { return ESP_ERR_NOT_FOUND; }
    if (running!=selected) { return ESP_ERR_INVALID_STATE; }
    // FINISH only prepares. Cancellation never rewrites boot metadata, so a
    // power loss cannot turn the confirmed baseline into a NEW image.
    d.target=nullptr;d.finished=false;return kOk;
}
Err Ota::confirm() {
    OtaBootInfo info;const auto err=this->info(info);if (err!=kOk) { return err; }
    return info.pending ? esp_ota_mark_app_valid_cancel_rollback() : kOk;
}
}
