#include "imu_bmo_055.hpp"

#include "bno055_device.hpp"
#include "tick_util.hpp"
#include "wall_clock.hpp"
#include "wire_codec.hpp"

extern "C" {
#include "bno055.h"
}

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <algorithm>
#include <cinttypes>

namespace imu_bmo_055 {

namespace {

static const char *const TAG = "imu_bno055";
constexpr TickType_t kMutexTimeout = rtos_util::msToTicksCeil(CONFIG_IMU_BMO_055_MUTEX_TIMEOUT_MS);
constexpr size_t kMaxResponseExtraPayload = 64U;
constexpr size_t kMaxStreamPayload = CONFIG_UART_COMM_MAX_PAYLOAD;
// The stream task waits for the timer at most 2 periods + this margin before
// it reports that the timer is not firing.
constexpr uint32_t kWakeTimeoutMarginMs = 100U;
constexpr int64_t kStreamLogIntervalUs = 1000000;

bool isValidOperationMode(uint8_t mode)
{
    return mode <= BNO055_OPERATION_MODE_NDOF;
}

void encodeCalibrationBlob(const CalibrationBlob &blob, uint8_t *out, size_t cap, uint8_t &out_len)
{
    out_len = 0;
    uart_comm::wire::Writer w(out, cap);
    w.appendU8(blob.has_sic ? 1U : 0U);
    w.appendI16Le(blob.accel_offset_x);
    w.appendI16Le(blob.accel_offset_y);
    w.appendI16Le(blob.accel_offset_z);
    w.appendI16Le(blob.accel_radius);

    w.appendI16Le(blob.gyro_offset_x);
    w.appendI16Le(blob.gyro_offset_y);
    w.appendI16Le(blob.gyro_offset_z);

    w.appendI16Le(blob.mag_offset_x);
    w.appendI16Le(blob.mag_offset_y);
    w.appendI16Le(blob.mag_offset_z);
    w.appendI16Le(blob.mag_radius);

    for (size_t i = 0; i < 9U; ++i) {
        w.appendI16Le(blob.sic[i]);
    }

    if (w.ok()) {
        out_len = static_cast<uint8_t>(w.size());
    }
}

/// Decode an exactly kCalibrationBlobSize-byte blob. Rejects a has_sic byte
/// other than 0 or 1.
bool decodeCalibrationBlob(const uint8_t *payload, size_t payload_len, CalibrationBlob &out)
{
    if (payload_len != kCalibrationBlobSize) {
        return false;
    }

    uart_comm::wire::Reader r(payload, payload_len);
    const uint8_t has_sic = r.readU8();
    if (has_sic > 1U) {
        return false;
    }

    CalibrationBlob blob{};
    blob.has_sic = (has_sic != 0U);
    blob.accel_offset_x = r.readI16Le();
    blob.accel_offset_y = r.readI16Le();
    blob.accel_offset_z = r.readI16Le();
    blob.accel_radius = r.readI16Le();

    blob.gyro_offset_x = r.readI16Le();
    blob.gyro_offset_y = r.readI16Le();
    blob.gyro_offset_z = r.readI16Le();

    blob.mag_offset_x = r.readI16Le();
    blob.mag_offset_y = r.readI16Le();
    blob.mag_offset_z = r.readI16Le();
    blob.mag_radius = r.readI16Le();

    for (size_t i = 0; i < 9U; ++i) {
        blob.sic[i] = r.readI16Le();
    }

    if (!r.done()) {
        return false;
    }
    out = blob;
    return true;
}

bool encodeStreamSample(const ImuStreamSample &sample,
                        uint16_t field_mask,
                        uint8_t *out,
                        size_t cap,
                        uint8_t &out_len)
{
    out_len = 0;
    uart_comm::wire::Writer w(out, cap);
    w.appendU8(kProtocolVersion);
    w.appendU16Le(field_mask);
    w.appendU64Le(sample.timestamp_us);

    const auto append_xyz = [&w](int16_t x, int16_t y, int16_t z) {
        w.appendI16Le(x);
        w.appendI16Le(y);
        w.appendI16Le(z);
    };

    if ((field_mask & static_cast<uint16_t>(StreamField::Euler)) != 0U) {
        append_xyz(sample.euler.h, sample.euler.r, sample.euler.p);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Quaternion)) != 0U) {
        w.appendI16Le(sample.quaternion.w);
        w.appendI16Le(sample.quaternion.x);
        w.appendI16Le(sample.quaternion.y);
        w.appendI16Le(sample.quaternion.z);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::LinearAccel)) != 0U) {
        append_xyz(sample.linear_accel.x, sample.linear_accel.y, sample.linear_accel.z);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Gravity)) != 0U) {
        append_xyz(sample.gravity.x, sample.gravity.y, sample.gravity.z);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::RawAccel)) != 0U) {
        append_xyz(sample.raw_accel.x, sample.raw_accel.y, sample.raw_accel.z);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::RawGyro)) != 0U) {
        append_xyz(sample.raw_gyro.x, sample.raw_gyro.y, sample.raw_gyro.z);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::RawMag)) != 0U) {
        append_xyz(sample.raw_mag.x, sample.raw_mag.y, sample.raw_mag.z);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Temperature)) != 0U) {
        w.appendU8(static_cast<uint8_t>(sample.temperature));
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::CalibrationStatus)) != 0U) {
        w.appendU8(sample.calibration_status.sys);
        w.appendU8(sample.calibration_status.gyro);
        w.appendU8(sample.calibration_status.accel);
        w.appendU8(sample.calibration_status.mag);
    }

    if (!w.ok() || w.size() > static_cast<size_t>(UINT8_MAX)) {
        return false;
    }

    out_len = static_cast<uint8_t>(w.size());
    return true;
}

int16_t randomS16(int16_t min_value, int16_t max_value)
{
    if (max_value <= min_value) {
        return min_value;
    }

    const int32_t span = static_cast<int32_t>(max_value) - static_cast<int32_t>(min_value) + 1;
    const int32_t value = static_cast<int32_t>(min_value) + static_cast<int32_t>(esp_random() % static_cast<uint32_t>(span));
    return static_cast<int16_t>(value);
}

uint8_t randomU8(uint8_t min_value, uint8_t max_value)
{
    if (max_value <= min_value) {
        return min_value;
    }

    const uint32_t span = static_cast<uint32_t>(max_value - min_value) + 1U;
    return static_cast<uint8_t>(min_value + static_cast<uint8_t>(esp_random() % span));
}

void populateMockSample(uint16_t field_mask, ImuStreamSample &sample)
{
    sample.timestamp_us = detail::wallClockUs();

    if ((field_mask & static_cast<uint16_t>(StreamField::Euler)) != 0U) {
        sample.euler.h = randomS16(0, 5760);
        sample.euler.r = randomS16(-2880, 2880);
        sample.euler.p = randomS16(-1440, 1440);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Quaternion)) != 0U) {
        sample.quaternion.w = randomS16(12000, 16384);
        sample.quaternion.x = randomS16(-2000, 2000);
        sample.quaternion.y = randomS16(-2000, 2000);
        sample.quaternion.z = randomS16(-2000, 2000);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::LinearAccel)) != 0U) {
        sample.linear_accel.x = randomS16(-200, 200);
        sample.linear_accel.y = randomS16(-200, 200);
        sample.linear_accel.z = randomS16(-200, 200);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Gravity)) != 0U) {
        sample.gravity.x = randomS16(-100, 100);
        sample.gravity.y = randomS16(-100, 100);
        sample.gravity.z = randomS16(950, 1050);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::RawAccel)) != 0U) {
        sample.raw_accel.x = randomS16(-1500, 1500);
        sample.raw_accel.y = randomS16(-1500, 1500);
        sample.raw_accel.z = randomS16(-1500, 1500);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::RawGyro)) != 0U) {
        sample.raw_gyro.x = randomS16(-700, 700);
        sample.raw_gyro.y = randomS16(-700, 700);
        sample.raw_gyro.z = randomS16(-700, 700);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::RawMag)) != 0U) {
        sample.raw_mag.x = randomS16(-800, 800);
        sample.raw_mag.y = randomS16(-800, 800);
        sample.raw_mag.z = randomS16(-800, 800);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Temperature)) != 0U) {
        sample.temperature = static_cast<int8_t>(randomS16(24, 38));
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::CalibrationStatus)) != 0U) {
        sample.calibration_status.sys = randomU8(2, 3);
        sample.calibration_status.gyro = randomU8(2, 3);
        sample.calibration_status.accel = randomU8(1, 3);
        sample.calibration_status.mag = randomU8(1, 3);
    }
}

/// Telemetry divider N: publish every N samples. The requested telemetry
/// period (0 = same as the sample period) is clamped to
/// [sample_period_us, max_period_us] and rounded up to a whole multiple.
uint32_t telemetryDivider(uint32_t requested_telemetry_us, uint32_t sample_period_us, uint32_t max_period_us)
{
    const uint32_t tp = std::clamp(requested_telemetry_us, sample_period_us, std::max(sample_period_us, max_period_us));
    return std::max<uint32_t>(1U, (tp + sample_period_us - 1U) / sample_period_us);
}

ResultCode deviceErrToResult(esp_err_t err)
{
    if (err == ESP_OK) {
        return ResultCode::Ok;
    }
    // ESP_ERR_TIMEOUT: the device lock is held by another operation.
    return (err == ESP_ERR_TIMEOUT) ? ResultCode::Busy : ResultCode::InternalError;
}

} // namespace

ImuBno055Manager::ImuBno055Manager(uart_comm::UartComm &uart, const ManagerConfig &config)
    : m_uart(uart),
      m_config(config),
      m_mutex(xSemaphoreCreateMutex()),
      m_config_queue(nullptr),
      m_valid(false),
      m_stream_enabled(false),
      m_field_mask(kStreamFieldAll),
      m_sample_period_us(0),
      m_telemetry_divider(1),
      m_stream_restarted(true),
      m_requested_telemetry_period_us(0),
      m_operation_mode(0),
      m_late_count(0),
      m_busy_count(0),
      m_read_error_count(0),
      m_encode_error_count(0),
      m_cached_calibration(),
      m_has_cached_calibration(false),
      m_use_mock_data(false),
      m_device(nullptr)
{
    if (m_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    if (!m_uart.isValid()) {
        ESP_LOGE(TAG, "uart_comm instance is invalid");
        return;
    }

    // Validate the period bounds before clamping with them (std::clamp is
    // undefined when lo > hi).
    if (m_config.min_sample_period_us == 0U || m_config.min_sample_period_us > m_config.max_sample_period_us) {
        ESP_LOGE(TAG, "Invalid sample period bounds: min=%" PRIu32 " max=%" PRIu32 " us",
                 m_config.min_sample_period_us, m_config.max_sample_period_us);
        return;
    }

    const uint32_t sample_period_us = std::clamp(m_config.stream_task.sample_period_us,
                                                 m_config.min_sample_period_us,
                                                 m_config.max_sample_period_us);
    if (sample_period_us != m_config.stream_task.sample_period_us) {
        ESP_LOGW(TAG, "sample_period_us %" PRIu32 " clamped to %" PRIu32,
                 m_config.stream_task.sample_period_us, sample_period_us);
    }
    m_sample_period_us.store(sample_period_us, std::memory_order_relaxed);
    m_requested_telemetry_period_us = m_config.stream_task.telemetry_period_us;
    m_telemetry_divider.store(
        telemetryDivider(m_requested_telemetry_period_us, sample_period_us, m_config.max_sample_period_us),
        std::memory_order_relaxed);

    uint16_t field_mask = m_config.default_stream_field_mask;
    if ((field_mask & static_cast<uint16_t>(~kStreamFieldAll)) != 0U) {
        ESP_LOGW(TAG, "Default field mask 0x%04X has unknown bits; using 0x%04X",
                 field_mask, static_cast<unsigned>(field_mask & kStreamFieldAll));
        field_mask &= kStreamFieldAll;
    }
    m_field_mask.store(field_mask, std::memory_order_relaxed);
    m_stream_enabled.store(m_config.stream_task.enable, std::memory_order_relaxed);
    m_operation_mode = m_config.default_operation_mode;

    if (!isValidOperationMode(m_operation_mode)) {
        ESP_LOGE(TAG, "Invalid default operation mode: 0x%02X", m_operation_mode);
        return;
    }

    m_use_mock_data = m_config.use_mock_data;
    if (!m_use_mock_data) {
        m_device = std::make_unique<Bno055Device>(m_config.i2c);
        if (!m_device->isValid()) {
            if (m_config.fallback_to_mock_on_init_failure) {
                ESP_LOGW(TAG, "BNO055 init failed; falling back to mock telemetry");
                m_device.reset();
                m_use_mock_data = true;
            } else {
                ESP_LOGE(TAG, "Failed to initialize BNO055 device");
                return;
            }
        }
    }

    if (!m_use_mock_data) {
        const esp_err_t mode_ret = m_device->setOperationMode(m_operation_mode);
        if (mode_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set default operation mode 0x%02X: %s",
                     m_operation_mode, esp_err_to_name(mode_ret));
            return;
        }
    } else {
        ESP_LOGW(TAG, "IMU mock mode enabled; streaming synthesized BNO055-like data");
    }

    m_config_queue = xQueueCreate(m_config.config_queue_depth, sizeof(uart_comm::RxFrame));
    if (m_config_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create config queue (depth=%u)", static_cast<unsigned>(m_config.config_queue_depth));
        return;
    }

    const esp_err_t cb_ret = m_uart.registerCallback(
        m_config.configure_msg_type,
        [this](const uart_comm::RxFrame &frame) {
            this->handleConfigureCommand(frame);
        },
        m_configure_cb);
    if (cb_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register configure callback (msg=0x%02X): %s",
                 m_config.configure_msg_type,
                 esp_err_to_name(cb_ret));
        return;
    }

    const esp_err_t cfg_task_ret = m_config_task.start(
        "imu_cfg",
        m_config.config_task_stack_size,
        m_config.config_task_priority,
        [this] { configTaskLoop(); });
    if (cfg_task_ret != ESP_OK) {
        m_configure_cb.reset();
        ESP_LOGE(TAG, "Failed to create config task: %s", esp_err_to_name(cfg_task_ret));
        return;
    }

    if (m_config.stream_task.enable) {
        const esp_err_t stream_task_ret = m_stream_task.start(
            "imu_stream",
            m_config.stream_task.stack_size,
            m_config.stream_task.priority,
            [this] { streamTaskLoop(); });
        if (stream_task_ret != ESP_OK) {
            m_configure_cb.reset();
            stopTasks();
            ESP_LOGE(TAG, "Failed to create stream task: %s", esp_err_to_name(stream_task_ret));
            return;
        }

        // Start the timer only once the task it wakes exists.
        esp_err_t tmr_ret = m_stream_timer.create(m_stream_task, "imu_sample");
        if (tmr_ret == ESP_OK) {
            tmr_ret = m_stream_timer.setPeriod(sample_period_us);
        }
        if (tmr_ret == ESP_OK && m_stream_enabled.load(std::memory_order_relaxed)) {
            tmr_ret = m_stream_timer.start(sample_period_us);
        }
        if (tmr_ret != ESP_OK) {
            m_configure_cb.reset();
            stopTasks();
            ESP_LOGE(TAG, "Failed to start sampling timer: %s", esp_err_to_name(tmr_ret));
            return;
        }
    }

    m_valid.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "IMU manager ready (cmd=0x%02X rsp=0x%02X stream=0x%02X sample=%" PRIu32
                  " us telemetry every %" PRIu32 " samples mask=0x%04X source=%s)",
             m_config.configure_msg_type,
             m_config.configure_response_msg_type,
             m_config.telemetry_stream_channel,
             sample_period_us,
             m_telemetry_divider.load(std::memory_order_relaxed),
             field_mask,
             m_use_mock_data ? "mock" : "bno055");
}

ImuBno055Manager::~ImuBno055Manager()
{
    // Unregister first. reset() waits for a callback that is already running
    // on the UART RX task, so none can touch this object afterwards.
    m_configure_cb.reset();
    stopTasks();

    if (m_config_queue != nullptr) {
        vQueueDelete(m_config_queue);
        m_config_queue = nullptr;
    }

    if (m_mutex != nullptr) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
}

bool ImuBno055Manager::isValid() const
{
    return m_valid.load(std::memory_order_acquire);
}

ResultCode ImuBno055Manager::applyStreamEnabledLocked(bool enabled)
{
    const bool was_enabled = m_stream_enabled.load(std::memory_order_relaxed);
    if (enabled == was_enabled) {
        return ResultCode::Ok;
    }

    if (!enabled) {
        m_stream_enabled.store(false, std::memory_order_release);
        // A callback already in flight only wakes the task, which then sees
        // the stream disabled and does nothing.
        return (m_stream_timer.stop() == ESP_OK) ? ResultCode::Ok : ResultCode::InternalError;
    }

    m_stream_restarted.store(true, std::memory_order_release);
    m_stream_enabled.store(true, std::memory_order_release);
    if (m_stream_timer.isCreated() &&
        m_stream_timer.start(m_sample_period_us.load(std::memory_order_relaxed)) != ESP_OK) {
        m_stream_enabled.store(false, std::memory_order_release);
        return ResultCode::InternalError;
    }
    return ResultCode::Ok;
}

ResultCode ImuBno055Manager::setStreamEnabled(bool enabled)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }
    const ResultCode rc = applyStreamEnabledLocked(enabled);
    xSemaphoreGive(m_mutex);
    return rc;
}

ResultCode ImuBno055Manager::setStreamTiming(uint32_t sample_period_us,
                                             uint32_t telemetry_period_us,
                                             uint32_t &applied_sample_period_us,
                                             uint32_t &applied_telemetry_period_us)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }

    const uint32_t old_period = m_sample_period_us.load(std::memory_order_relaxed);
    const uint32_t new_period = (sample_period_us != 0U)
                                    ? std::clamp(sample_period_us, m_config.min_sample_period_us,
                                                 m_config.max_sample_period_us)
                                    : old_period;
    if (telemetry_period_us != 0U) {
        m_requested_telemetry_period_us = telemetry_period_us;
    }

    ResultCode rc = ResultCode::Ok;
    if (new_period != old_period && m_stream_timer.isCreated()) {
        const esp_err_t err = m_stream_timer.setPeriod(new_period);
        if (err != ESP_OK) {
            rc = ResultCode::InternalError;
        }
    }
    if (rc == ResultCode::Ok) {
        m_sample_period_us.store(new_period, std::memory_order_relaxed);
    }
    const uint32_t period_now = m_sample_period_us.load(std::memory_order_relaxed);
    const uint32_t divider = telemetryDivider(m_requested_telemetry_period_us, period_now,
                                              m_config.max_sample_period_us);
    m_telemetry_divider.store(divider, std::memory_order_relaxed);
    xSemaphoreGive(m_mutex);

    applied_sample_period_us = period_now;
    applied_telemetry_period_us = divider * period_now;
    ESP_LOGI(TAG, "Stream timing: sample=%" PRIu32 " us telemetry=%" PRIu32 " us (every %" PRIu32 " samples)",
             applied_sample_period_us, applied_telemetry_period_us, divider);
    return rc;
}

ResultCode ImuBno055Manager::setStreamFieldMask(uint16_t field_mask)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if ((field_mask & static_cast<uint16_t>(~kStreamFieldAll)) != 0U) {
        return ResultCode::InvalidArgument;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }
    m_field_mask.store(field_mask, std::memory_order_relaxed);
    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode ImuBno055Manager::setOperationMode(uint8_t operation_mode)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (!isValidOperationMode(operation_mode)) {
        return ResultCode::InvalidArgument;
    }

    if (m_use_mock_data) {
        if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
            return ResultCode::Busy;
        }
        m_operation_mode = operation_mode;
        xSemaphoreGive(m_mutex);
        return ResultCode::Ok;
    }

    // Pause the stream (and its timer) around the mode switch, which holds
    // the device for 20-600 ms.
    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }
    const bool resume_stream = m_stream_enabled.load(std::memory_order_relaxed);
    (void)applyStreamEnabledLocked(false);
    xSemaphoreGive(m_mutex);

    const esp_err_t ret = m_device->setOperationMode(operation_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set operation mode 0x%02X: %s",
                 operation_mode,
                 esp_err_to_name(ret));
    }

    ResultCode rc = deviceErrToResult(ret);
    if (xSemaphoreTake(m_mutex, kMutexTimeout) == pdTRUE) {
        if (ret == ESP_OK) {
            m_operation_mode = operation_mode;
        }
        if (resume_stream && applyStreamEnabledLocked(true) != ResultCode::Ok && rc == ResultCode::Ok) {
            rc = ResultCode::InternalError;
        }
        xSemaphoreGive(m_mutex);
    } else if (resume_stream) {
        ESP_LOGE(TAG, "Could not re-enable the stream after the mode switch (mutex busy)");
        if (rc == ResultCode::Ok) {
            rc = ResultCode::Busy;
        }
    }

    return rc;
}

ResultCode ImuBno055Manager::readCalibration(CalibrationBlob &out)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (m_use_mock_data) {
        if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
            return ResultCode::Busy;
        }
        if (!m_has_cached_calibration) {
            m_cached_calibration = CalibrationBlob{};
            m_has_cached_calibration = true;
        }
        out = m_cached_calibration;
        xSemaphoreGive(m_mutex);
        return ResultCode::Ok;
    }

    const esp_err_t ret = m_device->readCalibration(out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read calibration: %s", esp_err_to_name(ret));
        return deviceErrToResult(ret);
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }
    m_cached_calibration = out;
    m_has_cached_calibration = true;
    xSemaphoreGive(m_mutex);

    return ResultCode::Ok;
}

ResultCode ImuBno055Manager::writeCalibration(const CalibrationBlob &blob)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }
    m_cached_calibration = blob;
    m_has_cached_calibration = true;
    xSemaphoreGive(m_mutex);

    return ResultCode::Ok;
}

ResultCode ImuBno055Manager::applyCalibration()
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    CalibrationBlob blob{};
    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }

    if (!m_has_cached_calibration) {
        xSemaphoreGive(m_mutex);
        return ResultCode::InvalidState;
    }
    blob = m_cached_calibration;
    xSemaphoreGive(m_mutex);

    if (m_use_mock_data) {
        return ResultCode::Ok;
    }

    const esp_err_t ret = m_device->writeCalibration(blob);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply calibration: %s", esp_err_to_name(ret));
        return deviceErrToResult(ret);
    }
    return ResultCode::Ok;
}

StreamCounters ImuBno055Manager::getStreamCounters() const
{
    StreamCounters c{};
    c.late = m_late_count.load(std::memory_order_relaxed);
    c.busy = m_busy_count.load(std::memory_order_relaxed);
    c.read_errors = m_read_error_count.load(std::memory_order_relaxed);
    c.encode_errors = m_encode_error_count.load(std::memory_order_relaxed);
    return c;
}

void ImuBno055Manager::handleConfigureCommand(const uart_comm::RxFrame &frame)
{
    if (!isValid() || m_config_queue == nullptr) {
        return;
    }

    if (xQueueSend(m_config_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Config queue full; dropping command");
    }
}

ResultCode ImuBno055Manager::parseAndDispatchConfigure(ConfigureSubcommand subcommand,
                                                       const uint8_t *body,
                                                       uint8_t len,
                                                       uint8_t &response_len,
                                                       uint8_t *response_payload,
                                                       size_t response_capacity)
{
    // body is the payload after the configure request header; len is its
    // length. Every body must have its exact size (protocol v3).
    response_len = 0;

    switch (subcommand) {
        case ConfigureSubcommand::SetStreamEnabled: {
            if (len != kSetStreamEnabledBodySize || body[0] > 1U) {
                return ResultCode::InvalidArgument;
            }
            return setStreamEnabled(body[0] != 0U);
        }
        case ConfigureSubcommand::SetStreamTiming: {
            if (len != kSetStreamTimingBodySize) {
                return ResultCode::InvalidArgument;
            }
            uart_comm::wire::Reader r(body, len);
            const uint32_t sample_us = r.readU32Le();
            const uint32_t telemetry_us = r.readU32Le();
            if (!r.done()) {
                return ResultCode::InvalidArgument;
            }
            uint32_t applied_sample_us = 0;
            uint32_t applied_telemetry_us = 0;
            const ResultCode rc = setStreamTiming(sample_us, telemetry_us, applied_sample_us, applied_telemetry_us);
            if (rc == ResultCode::Ok) {
                uart_comm::wire::Writer w(response_payload, response_capacity);
                w.appendU32Le(applied_sample_us);
                w.appendU32Le(applied_telemetry_us);
                if (!w.ok()) {
                    return ResultCode::InternalError;
                }
                response_len = static_cast<uint8_t>(w.size());
            }
            return rc;
        }
        case ConfigureSubcommand::SetStreamFieldMask: {
            if (len != kSetStreamFieldMaskBodySize) {
                return ResultCode::InvalidArgument;
            }
            uart_comm::wire::Reader r(body, len);
            const uint16_t mask = r.readU16Le();
            if (!r.done()) {
                return ResultCode::InvalidArgument;
            }
            return setStreamFieldMask(mask);
        }
        case ConfigureSubcommand::SetOperationMode: {
            if (len != kSetOperationModeBodySize) {
                return ResultCode::InvalidArgument;
            }
            return setOperationMode(body[0]);
        }
        case ConfigureSubcommand::ReadCalibration: {
            if (len != 0U) {
                return ResultCode::InvalidArgument;
            }
            CalibrationBlob blob{};
            const ResultCode rc = readCalibration(blob);
            if (rc != ResultCode::Ok) {
                return rc;
            }
            if (response_capacity < kCalibrationBlobSize) {
                return ResultCode::InternalError;
            }
            encodeCalibrationBlob(blob, response_payload, response_capacity, response_len);
            return ResultCode::Ok;
        }
        case ConfigureSubcommand::WriteCalibration: {
            CalibrationBlob blob{};
            if (!decodeCalibrationBlob(body, len, blob)) {
                return ResultCode::InvalidArgument;
            }
            return writeCalibration(blob);
        }
        case ConfigureSubcommand::ApplyCalibration:
            if (len != 0U) {
                return ResultCode::InvalidArgument;
            }
            return applyCalibration();
        default:
            return ResultCode::NotSupported;
    }
}

esp_err_t ImuBno055Manager::sendConfigureResponse(uint8_t seq,
                                                  uint8_t subcommand,
                                                  ResultCode code,
                                                  const uint8_t *extra_payload,
                                                  uint8_t extra_len)
{
    uint8_t payload[kConfigureResponseHeaderSize + kMaxResponseExtraPayload] = {0};
    uart_comm::wire::Writer w(payload, sizeof(payload));
    w.appendU8(kProtocolVersion);
    w.appendU8(seq);
    w.appendU8(subcommand);
    w.appendU8(static_cast<uint8_t>(code));
    w.appendBytes(extra_payload, extra_len);
    if (!w.ok()) {
        return ESP_ERR_INVALID_ARG;
    }

    return m_uart.sendCommand(m_config.configure_response_msg_type, payload,
                              static_cast<uint8_t>(w.size()));
}

void ImuBno055Manager::stopTasks()
{
    m_stream_task.requestStop();
    m_config_task.requestStop();

    if (m_config_queue != nullptr && m_config_task.isRunning()) {
        // Wake the config task out of xQueueReceive. If the queue is full,
        // its 100 ms receive timeout wakes it instead.
        uart_comm::RxFrame wake{};
        (void)xQueueSend(m_config_queue, &wake, 0);
    }

    // Join the config task first: it is the only other user of the timer
    // (enable/disable/retime), and TaskTimer calls are not thread-safe.
    m_config_task.stop();
    // Then the timer, waiting out an in-flight callback, so nothing notifies
    // the stream task while it is being stopped and deleted.
    m_stream_timer.destroy();
    // Neither task is ever deleted while it holds m_mutex or is inside an
    // I2C transaction.
    m_stream_task.stop();

    m_valid.store(false, std::memory_order_release);
}

void ImuBno055Manager::logStreamCountersIfChanged(StreamCounters &last_logged, int64_t &last_log_us)
{
    const int64_t now_us = esp_timer_get_time();
    if ((now_us - last_log_us) < kStreamLogIntervalUs) {
        return;
    }
    const StreamCounters now = getStreamCounters();
    if (now.late == last_logged.late && now.busy == last_logged.busy &&
        now.read_errors == last_logged.read_errors && now.encode_errors == last_logged.encode_errors) {
        return;
    }
    ESP_LOGW(TAG, "IMU stream: late +%" PRIu32 " busy +%" PRIu32 " read_err +%" PRIu32 " encode_err +%" PRIu32
                  " (totals %" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 ") sample=%" PRIu32 " us",
             now.late - last_logged.late, now.busy - last_logged.busy,
             now.read_errors - last_logged.read_errors, now.encode_errors - last_logged.encode_errors,
             now.late, now.busy, now.read_errors, now.encode_errors,
             m_sample_period_us.load(std::memory_order_relaxed));
    last_logged = now;
    last_log_us = now_us;
}

void ImuBno055Manager::streamTaskLoop()
{
    uint8_t tx_payload[kMaxStreamPayload] = {0};
    // Samples until the next publish; <= 0 means publish this sample.
    int32_t countdown = 0;
    StreamCounters last_logged{};
    int64_t last_counter_log_us = 0;
    int64_t last_timeout_log_us = 0;

    while (!m_stream_task.stopRequested()) {
        // Woken by the sampling timer (and by stop()). The count is the
        // number of timer periods since we last took the notification.
        const uint32_t period_us = m_sample_period_us.load(std::memory_order_relaxed);
        const TickType_t wait = rtos_util::msToTicksCeil((2U * (period_us / 1000U)) + kWakeTimeoutMarginMs);
        const uint32_t fired = ulTaskNotifyTake(pdTRUE, wait);
        if (m_stream_task.stopRequested()) {
            break;
        }

        const bool enabled = m_stream_enabled.load(std::memory_order_acquire);
        if (fired == 0U) {
            // While disabled the timer is stopped, so a timeout is normal.
            const int64_t now_us = esp_timer_get_time();
            if (enabled && (now_us - last_timeout_log_us) >= kStreamLogIntervalUs) {
                last_timeout_log_us = now_us;
                ESP_LOGW(TAG, "Sampling timer has not fired for %" PRIu32 " ms",
                         static_cast<uint32_t>(pdTICKS_TO_MS(wait)));
            }
            continue;
        }
        if (!enabled) {
            continue; // a late callback after disable
        }

        uint32_t elapsed = fired;
        if (m_stream_restarted.exchange(false, std::memory_order_acq_rel)) {
            // First tick after (re-)enable: publish it, and don't count the
            // idle time as late ticks.
            countdown = 0;
            elapsed = 1U;
        }
        if (elapsed > 1U) {
            m_late_count.fetch_add(elapsed - 1U, std::memory_order_relaxed);
        }
        // Late ticks still count toward the telemetry countdown, so the
        // telemetry rate holds when samples are missed.
        countdown -= static_cast<int32_t>(std::min<uint32_t>(elapsed, static_cast<uint32_t>(INT32_MAX)));

        const uint16_t field_mask = m_field_mask.load(std::memory_order_relaxed);
        ImuStreamSample sample{};
        esp_err_t read_ret = ESP_ERR_INVALID_STATE;
        if (m_use_mock_data) {
            populateMockSample(field_mask, sample);
            read_ret = ESP_OK;
        } else if (m_device != nullptr) {
            read_ret = m_device->tryReadSample(field_mask, sample);
        }

        if (read_ret == ESP_ERR_TIMEOUT) {
            m_busy_count.fetch_add(1U, std::memory_order_relaxed);
        } else if (read_ret != ESP_OK) {
            m_read_error_count.fetch_add(1U, std::memory_order_relaxed);
        } else if (countdown <= 0) {
            // A publish slot whose read failed stays pending (countdown
            // stays <= 0), so the next good sample is published.
            const int32_t divider = static_cast<int32_t>(m_telemetry_divider.load(std::memory_order_relaxed));
            countdown = std::max<int32_t>(countdown + divider, 1);

            uint8_t tx_len = 0;
            if (encodeStreamSample(sample, field_mask, tx_payload, sizeof(tx_payload), tx_len)) {
                m_uart.sendData(m_config.telemetry_stream_channel, tx_payload, tx_len);
            } else {
                m_encode_error_count.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        // Samples read on non-publish ticks are dropped for now. This is
        // where an on-board consumer (latest-sample accessor, filter) would
        // take every sample when sampling faster than the telemetry rate.

        logStreamCountersIfChanged(last_logged, last_counter_log_us);
    }
}

void ImuBno055Manager::configTaskLoop()
{
    ImuBno055Manager *const self = this;

    while (!m_config_task.stopRequested()) {
        uart_comm::RxFrame frame{};
        if (xQueueReceive(self->m_config_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (m_config_task.stopRequested()) {
            // Woken by stopTasks(): this is its wake-up frame (or a late
            // command). Don't dispatch it or reply to the host during shutdown.
            break;
        }

        uint8_t response_extra[kMaxResponseExtraPayload] = {0};
        uint8_t response_extra_len = 0;
        uint8_t seq = 0;
        uint8_t subcommand_raw = kUnparsedHeaderByte;
        ResultCode rc = ResultCode::InvalidArgument;

        const bool version_ok = (frame.len >= 1U) && (frame.payload[0] == kProtocolVersion);
        if (version_ok && frame.len >= kConfigureRequestHeaderSize) {
            seq = frame.payload[1];
            subcommand_raw = frame.payload[2];
            rc = self->parseAndDispatchConfigure(
                static_cast<ConfigureSubcommand>(subcommand_raw),
                &frame.payload[kConfigureRequestHeaderSize],
                static_cast<uint8_t>(frame.len - kConfigureRequestHeaderSize),
                response_extra_len,
                response_extra,
                sizeof(response_extra));
        } else {
            // The header layout is unknown, so the subcommand cannot be
            // echoed. seq is echoed when the version matches and it is present.
            if (version_ok && frame.len >= 2U) {
                seq = frame.payload[1];
            }
            ESP_LOGW(TAG, "Configure header rejected: len=%u version=%u expected=%u",
                     frame.len, (frame.len >= 1U) ? frame.payload[0] : 0U, kProtocolVersion);
        }

        const esp_err_t send_ret = self->sendConfigureResponse(seq, subcommand_raw, rc, response_extra, response_extra_len);
        if (send_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send IMU configure response: %s", esp_err_to_name(send_ret));
        }
    }
}

const char *resultCodeToString(ResultCode code)
{
    switch (code) {
        case ResultCode::Ok:
            return "OK";
        case ResultCode::InvalidArgument:
            return "INVALID_ARGUMENT";
        case ResultCode::InvalidState:
            return "INVALID_STATE";
        case ResultCode::NotSupported:
            return "NOT_SUPPORTED";
        case ResultCode::Busy:
            return "BUSY";
        case ResultCode::InternalError:
        default:
            return "INTERNAL_ERROR";
    }
}

} // namespace imu_bmo_055
