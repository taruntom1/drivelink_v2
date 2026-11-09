#include "bno055_device.hpp"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "tick_util.hpp"
#include "wall_clock.hpp"

#include <cinttypes>
#include <cstring>

namespace imu_bmo_055 {

namespace {

static const char *TAG = "bno055_dev";
constexpr uint8_t kExpectedChipId = 0xA0;
// Config operations (mode switch, calibration) wait this long for the device
// lock. The stream tick holds it for a few ms at most and never waits.
constexpr TickType_t kConfigLockTimeout = pdMS_TO_TICKS(100);
constexpr int64_t kBusLogIntervalUs = 1000000;

esp_err_t boschToEspErr(s8 result)
{
    return (result == BNO055_SUCCESS) ? ESP_OK : ESP_FAIL;
}

} // namespace

Bno055Device *Bno055Device::s_active_device = nullptr;

Bno055Device::Bno055Device(const I2cConfig &config)
    : m_dev(),
      m_config(config),
      m_mutex(xSemaphoreCreateMutex()),
      m_valid(false),
      m_driver_installed(false),
      m_last_bus_log_us(0),
      m_suppressed_bus_logs(0)
{
    if (m_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate mutex");
        return;
    }

    if (s_active_device != nullptr) {
        ESP_LOGE(TAG, "Only one BNO055 instance is supported");
        return;
    }

    i2c_config_t i2c_cfg{};
    i2c_cfg.mode = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num = static_cast<gpio_num_t>(m_config.sda_pin);
    i2c_cfg.scl_io_num = static_cast<gpio_num_t>(m_config.scl_pin);
    i2c_cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = m_config.clk_speed_hz;

    const i2c_port_t port = static_cast<i2c_port_t>(m_config.port_num);
    esp_err_t ret = i2c_param_config(port, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed (port=%d sda=%d scl=%d): %s",
                 m_config.port_num,
                 m_config.sda_pin,
                 m_config.scl_pin,
                 esp_err_to_name(ret));
        return;
    }

    ret = i2c_driver_install(port, i2c_cfg.mode, 0, 0, 0);
    if (ret == ESP_OK) {
        m_driver_installed = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        m_driver_installed = false;
    } else {
        ESP_LOGE(TAG, "i2c_driver_install failed (port=%d): %s",
                 m_config.port_num,
                 esp_err_to_name(ret));
        return;
    }

    std::memset(&m_dev, 0, sizeof(m_dev));
    m_dev.dev_addr = m_config.device_address;
    m_dev.bus_write = &Bno055Device::busWriteThunk;
    m_dev.bus_read = &Bno055Device::busReadThunk;
    m_dev.delay_msec = &Bno055Device::delayThunk;

    // Give sensor a short window after powerup before first Bosch init
    delayThunk(100U);

    s_active_device = this;

    const s8 init_ret = bno055_init(&m_dev);
    if (init_ret != BNO055_SUCCESS) {
        ESP_LOGE(TAG, "bno055_init failed (addr=0x%02X)", m_config.device_address);
        s_active_device = nullptr;
        return;
    }

    if (m_dev.chip_id != kExpectedChipId) {
        ESP_LOGE(TAG, "Unexpected chip id: 0x%02X", m_dev.chip_id);
        s_active_device = nullptr;
        return;
    }

    m_valid = true;
    ESP_LOGI(TAG, "BNO055 ready (i2c_port=%d addr=0x%02X)", m_config.port_num, m_config.device_address);
}

Bno055Device::~Bno055Device()
{
    if (s_active_device == this) {
        s_active_device = nullptr;
    }

    if (m_driver_installed) {
        const i2c_port_t port = static_cast<i2c_port_t>(m_config.port_num);
        const esp_err_t ret = i2c_driver_delete(port);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2c_driver_delete failed (port=%d): %s",
                     m_config.port_num,
                     esp_err_to_name(ret));
        }
    }

    if (m_mutex != nullptr) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
}

bool Bno055Device::isValid() const
{
    return m_valid;
}

esp_err_t Bno055Device::setOperationMode(uint8_t operation_mode)
{
    if (!m_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t lock_ret = lock(kConfigLockTimeout);
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    const s8 ret = bno055_set_operation_mode(operation_mode);
    unlock();
    return boschToEspErr(ret);
}

esp_err_t Bno055Device::getOperationMode(uint8_t &operation_mode) const
{
    if (!m_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t lock_ret = lock(kConfigLockTimeout);
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    u8 mode = 0;
    const s8 ret = bno055_get_operation_mode(&mode);
    unlock();
    operation_mode = mode;
    return boschToEspErr(ret);
}

esp_err_t Bno055Device::tryReadSample(uint16_t field_mask, ImuStreamSample &sample) const
{
    if (!m_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    if (lock(0) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    // Stamp the midpoint of the reads. Wall time is read once; the burst
    // duration comes from the monotonic timer, so a clock step by time_sync
    // during the burst cannot skew it.
    const uint64_t wall_start_us = detail::wallClockUs();
    const int64_t mono_start_us = esp_timer_get_time();

    const auto has = [field_mask](StreamField field) {
        return (field_mask & static_cast<uint16_t>(field)) != 0U;
    };

    bool ok = true;
    if (ok && has(StreamField::Euler)) {
        ok = bno055_read_euler_hrp(&sample.euler) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::Quaternion)) {
        ok = bno055_read_quaternion_wxyz(&sample.quaternion) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::LinearAccel)) {
        ok = bno055_read_linear_accel_xyz(&sample.linear_accel) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::Gravity)) {
        ok = bno055_read_gravity_xyz(&sample.gravity) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::RawAccel)) {
        ok = bno055_read_accel_xyz(&sample.raw_accel) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::RawGyro)) {
        ok = bno055_read_gyro_xyz(&sample.raw_gyro) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::RawMag)) {
        ok = bno055_read_mag_xyz(&sample.raw_mag) == BNO055_SUCCESS;
    }
    if (ok && has(StreamField::Temperature)) {
        s8 temp = 0;
        ok = bno055_read_temp_data(&temp) == BNO055_SUCCESS;
        sample.temperature = static_cast<int8_t>(temp);
    }
    if (ok && has(StreamField::CalibrationStatus)) {
        u8 sys = 0;
        u8 gyro = 0;
        u8 accel = 0;
        u8 mag = 0;
        ok = bno055_get_sys_calib_stat(&sys) == BNO055_SUCCESS &&
             bno055_get_gyro_calib_stat(&gyro) == BNO055_SUCCESS &&
             bno055_get_accel_calib_stat(&accel) == BNO055_SUCCESS &&
             bno055_get_mag_calib_stat(&mag) == BNO055_SUCCESS;
        sample.calibration_status.sys = sys;
        sample.calibration_status.gyro = gyro;
        sample.calibration_status.accel = accel;
        sample.calibration_status.mag = mag;
    }

    const int64_t mono_end_us = esp_timer_get_time();
    unlock();

    sample.timestamp_us = wall_start_us + static_cast<uint64_t>((mono_end_us - mono_start_us) / 2);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t Bno055Device::readCalibration(CalibrationBlob &out) const
{
    if (!m_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t lock_ret = lock(kConfigLockTimeout);
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    bno055_accel_offset_t accel{};
    bno055_gyro_offset_t gyro{};
    bno055_mag_offset_t mag{};
    bno055_sic_matrix_t sic{};

    if (bno055_read_accel_offset(&accel) != BNO055_SUCCESS ||
        bno055_read_gyro_offset(&gyro) != BNO055_SUCCESS ||
        bno055_read_mag_offset(&mag) != BNO055_SUCCESS) {
        unlock();
        return ESP_FAIL;
    }

    out.accel_offset_x = accel.x;
    out.accel_offset_y = accel.y;
    out.accel_offset_z = accel.z;
    out.accel_radius = accel.r;

    out.gyro_offset_x = gyro.x;
    out.gyro_offset_y = gyro.y;
    out.gyro_offset_z = gyro.z;

    out.mag_offset_x = mag.x;
    out.mag_offset_y = mag.y;
    out.mag_offset_z = mag.z;
    out.mag_radius = mag.r;

    if (bno055_read_sic_matrix(&sic) == BNO055_SUCCESS) {
        out.sic[0] = sic.sic_0;
        out.sic[1] = sic.sic_1;
        out.sic[2] = sic.sic_2;
        out.sic[3] = sic.sic_3;
        out.sic[4] = sic.sic_4;
        out.sic[5] = sic.sic_5;
        out.sic[6] = sic.sic_6;
        out.sic[7] = sic.sic_7;
        out.sic[8] = sic.sic_8;
        out.has_sic = true;
    } else {
        out.has_sic = false;
    }

    unlock();
    return ESP_OK;
}

esp_err_t Bno055Device::writeCalibration(const CalibrationBlob &blob)
{
    if (!m_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t lock_ret = lock(kConfigLockTimeout);
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    u8 previous_mode = 0;
    if (bno055_get_operation_mode(&previous_mode) != BNO055_SUCCESS) {
        unlock();
        return ESP_FAIL;
    }

    if (previous_mode != BNO055_OPERATION_MODE_CONFIG &&
        bno055_set_operation_mode(BNO055_OPERATION_MODE_CONFIG) != BNO055_SUCCESS) {
        unlock();
        return ESP_FAIL;
    }

    bno055_accel_offset_t accel{};
    accel.x = blob.accel_offset_x;
    accel.y = blob.accel_offset_y;
    accel.z = blob.accel_offset_z;
    accel.r = blob.accel_radius;

    bno055_gyro_offset_t gyro{};
    gyro.x = blob.gyro_offset_x;
    gyro.y = blob.gyro_offset_y;
    gyro.z = blob.gyro_offset_z;

    bno055_mag_offset_t mag{};
    mag.x = blob.mag_offset_x;
    mag.y = blob.mag_offset_y;
    mag.z = blob.mag_offset_z;
    mag.r = blob.mag_radius;

    if (bno055_write_accel_offset(&accel) != BNO055_SUCCESS ||
        bno055_write_gyro_offset(&gyro) != BNO055_SUCCESS ||
        bno055_write_mag_offset(&mag) != BNO055_SUCCESS) {
        if (previous_mode != BNO055_OPERATION_MODE_CONFIG) {
            (void)bno055_set_operation_mode(previous_mode);
        }
        unlock();
        return ESP_FAIL;
    }

    if (blob.has_sic) {
        bno055_sic_matrix_t sic{};
        sic.sic_0 = blob.sic[0];
        sic.sic_1 = blob.sic[1];
        sic.sic_2 = blob.sic[2];
        sic.sic_3 = blob.sic[3];
        sic.sic_4 = blob.sic[4];
        sic.sic_5 = blob.sic[5];
        sic.sic_6 = blob.sic[6];
        sic.sic_7 = blob.sic[7];
        sic.sic_8 = blob.sic[8];
        if (bno055_write_sic_matrix(&sic) != BNO055_SUCCESS) {
            if (previous_mode != BNO055_OPERATION_MODE_CONFIG) {
                (void)bno055_set_operation_mode(previous_mode);
            }
            unlock();
            return ESP_FAIL;
        }
    }

    if (previous_mode != BNO055_OPERATION_MODE_CONFIG &&
        bno055_set_operation_mode(previous_mode) != BNO055_SUCCESS) {
        unlock();
        return ESP_FAIL;
    }

    unlock();
    return ESP_OK;
}

s8 Bno055Device::busWriteThunk(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
    (void)dev_addr;
    if (s_active_device == nullptr) {
        return BNO055_ERROR;
    }
    return s_active_device->busWrite(reg_addr, reg_data, cnt);
}

s8 Bno055Device::busReadThunk(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
    (void)dev_addr;
    if (s_active_device == nullptr) {
        return BNO055_ERROR;
    }
    return s_active_device->busRead(reg_addr, reg_data, cnt);
}

void Bno055Device::delayThunk(u32 delay_ms)
{
    if (delay_ms == 0U) {
        return;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        esp_rom_delay_us(static_cast<uint32_t>(delay_ms) * 1000U);
        return;
    }
    // Bosch delays are minimums (mode-switch settling). Round up to whole
    // ticks, plus one because vTaskDelay(n) can return up to a tick early.
    vTaskDelay(rtos_util::msToTicksCeil(delay_ms) + 1U);
}

s8 Bno055Device::busWrite(u8 reg_addr, const u8 *reg_data, u8 cnt) const
{
    uint8_t tx_buf[256] = {0};
    tx_buf[0] = reg_addr;
    if (cnt > 0U) {
        std::memcpy(&tx_buf[1], reg_data, cnt);
    }

    const i2c_port_t port = static_cast<i2c_port_t>(m_config.port_num);
    // Targeted retry for initial page-id write to mitigate intermittent
    // ESP_ERR_TIMEOUT during startup. 
    const bool is_page_id_write = (reg_addr == BNO055_PAGE_ID_REG && cnt == 1U);
    const unsigned attempts = is_page_id_write ? 3U : 1U;
    const uint32_t timeout_ms = is_page_id_write ? 500U : m_config.timeout_ms;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    unsigned used_attempts = 0;
    for (unsigned a = 1; a <= attempts; ++a) {
        ret = i2c_master_write_to_device(
            port,
            m_config.device_address,
            tx_buf,
            static_cast<size_t>(cnt) + 1U,
            rtos_util::msToTicksCeil(timeout_ms));
        used_attempts = a;
        if (ret == ESP_OK) {
            break;
        }
        if (a < attempts) {
            delayThunk(10U);
        }
    }

    if (ret == ESP_OK) {
        if (is_page_id_write && used_attempts > 1) {
            ESP_LOGI(TAG, "BNO055 page-id write recovered on attempt %u",
                     static_cast<unsigned>(used_attempts));
        }
        return BNO055_SUCCESS;
    }

    if (shouldLogBusError()) {
        ESP_LOGW(TAG,
                 "BNO055 bus write failed (port=%d addr=0x%02X reg=0x%02X data=0x%02X cnt=%u): %s "
                 "(%" PRIu32 " similar suppressed)",
                 m_config.port_num,
                 m_config.device_address,
                 reg_addr,
                 (cnt > 0U) ? tx_buf[1] : 0U,
                 static_cast<unsigned>(cnt),
                 esp_err_to_name(ret),
                 m_suppressed_bus_logs);
        m_suppressed_bus_logs = 0;
    }

    return BNO055_ERROR;
}

s8 Bno055Device::busRead(u8 reg_addr, u8 *reg_data, u8 cnt) const
{
    if (cnt == 0U) {
        return BNO055_SUCCESS;
    }

    const i2c_port_t port = static_cast<i2c_port_t>(m_config.port_num);
    const esp_err_t ret = i2c_master_write_read_device(
        port,
        m_config.device_address,
        &reg_addr,
        1,
        reg_data,
        cnt,
        rtos_util::msToTicksCeil(m_config.timeout_ms));

    if (ret != ESP_OK && shouldLogBusError()) {
        ESP_LOGW(TAG,
                 "BNO055 bus read failed (port=%d addr=0x%02X reg=0x%02X cnt=%u): %s "
                 "(%" PRIu32 " similar suppressed)",
                 m_config.port_num,
                 m_config.device_address,
                 reg_addr,
                 static_cast<unsigned>(cnt),
                 esp_err_to_name(ret),
                 m_suppressed_bus_logs);
        m_suppressed_bus_logs = 0;
    }

    return (ret == ESP_OK) ? BNO055_SUCCESS : BNO055_ERROR;
}

bool Bno055Device::shouldLogBusError() const
{
    const int64_t now_us = esp_timer_get_time();
    if (m_last_bus_log_us == 0 || (now_us - m_last_bus_log_us) >= kBusLogIntervalUs) {
        m_last_bus_log_us = now_us;
        return true;
    }
    ++m_suppressed_bus_logs;
    return false;
}

esp_err_t Bno055Device::lock(TickType_t timeout) const
{
    if (m_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreTake(m_mutex, timeout) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void Bno055Device::unlock() const
{
    if (m_mutex != nullptr) {
        xSemaphoreGive(m_mutex);
    }
}

} // namespace imu_bmo_055
