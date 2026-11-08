#include "imu_bmo_055.hpp"

#include "bno055_device.hpp"

extern "C" {
#include "bno055.h"
}

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstring>

namespace imu_bmo_055 {

namespace {

static const char *TAG = "imu_bno055";
constexpr TickType_t kMutexTimeout = pdMS_TO_TICKS(50);
constexpr size_t kMaxResponseExtraPayload = 64U;
constexpr size_t kMaxStreamPayload = CONFIG_UART_COMM_MAX_PAYLOAD;
constexpr size_t kCalibrationPayloadSize = 41U;

bool isValidOperationMode(uint8_t mode)
{
    return mode <= BNO055_OPERATION_MODE_NDOF;
}

void appendU16Le(uint8_t *dst, size_t cap, size_t &offset, uint16_t value)
{
    if (offset + 2U > cap) {
        return;
    }
    dst[offset++] = static_cast<uint8_t>(value & 0xFFU);
    dst[offset++] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void appendU64Le(uint8_t *dst, size_t cap, size_t &offset, uint64_t value)
{
    if (offset + 8U > cap) {
        return;
    }
    for (size_t i = 0; i < 8U; ++i) {
        dst[offset++] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
    }
}

void appendS16Le(uint8_t *dst, size_t cap, size_t &offset, int16_t value)
{
    appendU16Le(dst, cap, offset, static_cast<uint16_t>(value));
}

uint16_t readU16Le(const uint8_t *src, size_t src_len, size_t &offset, bool &ok)
{
    if ((offset + 2U) > src_len) {
        ok = false;
        return 0;
    }
    const uint16_t value = static_cast<uint16_t>(src[offset]) |
                           (static_cast<uint16_t>(src[offset + 1U]) << 8U);
    offset += 2U;
    return value;
}

void encodeCalibrationBlob(const CalibrationBlob &blob, uint8_t *out, size_t cap, uint8_t &out_len)
{
    out_len = 0;
    if (cap < kCalibrationPayloadSize) {
        return;
    }

    size_t offset = 0;
    out[offset++] = blob.has_sic ? 1U : 0U;
    appendS16Le(out, cap, offset, blob.accel_offset_x);
    appendS16Le(out, cap, offset, blob.accel_offset_y);
    appendS16Le(out, cap, offset, blob.accel_offset_z);
    appendS16Le(out, cap, offset, blob.accel_radius);

    appendS16Le(out, cap, offset, blob.gyro_offset_x);
    appendS16Le(out, cap, offset, blob.gyro_offset_y);
    appendS16Le(out, cap, offset, blob.gyro_offset_z);

    appendS16Le(out, cap, offset, blob.mag_offset_x);
    appendS16Le(out, cap, offset, blob.mag_offset_y);
    appendS16Le(out, cap, offset, blob.mag_offset_z);
    appendS16Le(out, cap, offset, blob.mag_radius);

    for (size_t i = 0; i < 9U; ++i) {
        appendS16Le(out, cap, offset, blob.sic[i]);
    }

    out_len = static_cast<uint8_t>(offset);
}

bool decodeCalibrationBlob(const uint8_t *payload, size_t payload_len, size_t &offset, CalibrationBlob &out)
{
    if ((offset + kCalibrationPayloadSize) > payload_len) {
        return false;
    }

    out.has_sic = payload[offset++] != 0U;

    bool ok = true;
    out.accel_offset_x = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.accel_offset_y = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.accel_offset_z = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.accel_radius = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));

    out.gyro_offset_x = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.gyro_offset_y = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.gyro_offset_z = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));

    out.mag_offset_x = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.mag_offset_y = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.mag_offset_z = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    out.mag_radius = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));

    for (size_t i = 0; i < 9U; ++i) {
        out.sic[i] = static_cast<int16_t>(readU16Le(payload, payload_len, offset, ok));
    }

    return ok;
}

bool encodeStreamSample(const ImuStreamSample &sample,
                        uint16_t field_mask,
                        uint8_t *out,
                        size_t cap,
                        uint8_t &out_len)
{
    out_len = 0;
    if (cap < kStreamHeaderSize) {
        return false;
    }

    size_t offset = 0;
    out[offset++] = kProtocolVersion;
    appendU16Le(out, cap, offset, field_mask);
    appendU64Le(out, cap, offset, sample.timestamp_us);

    const auto append_xyz = [&](int16_t x, int16_t y, int16_t z) {
        appendS16Le(out, cap, offset, x);
        appendS16Le(out, cap, offset, y);
        appendS16Le(out, cap, offset, z);
    };

    if ((field_mask & static_cast<uint16_t>(StreamField::Euler)) != 0U) {
        append_xyz(sample.euler.h, sample.euler.r, sample.euler.p);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::Quaternion)) != 0U) {
        appendS16Le(out, cap, offset, sample.quaternion.w);
        appendS16Le(out, cap, offset, sample.quaternion.x);
        appendS16Le(out, cap, offset, sample.quaternion.y);
        appendS16Le(out, cap, offset, sample.quaternion.z);
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
        if (offset + 1U > cap) {
            return false;
        }
        out[offset++] = static_cast<uint8_t>(sample.temperature);
    }
    if ((field_mask & static_cast<uint16_t>(StreamField::CalibrationStatus)) != 0U) {
        if (offset + 4U > cap) {
            return false;
        }
        out[offset++] = sample.calibration_status.sys;
        out[offset++] = sample.calibration_status.gyro;
        out[offset++] = sample.calibration_status.accel;
        out[offset++] = sample.calibration_status.mag;
    }

    if (offset > cap || offset > static_cast<size_t>(UINT8_MAX)) {
        return false;
    }

    out_len = static_cast<uint8_t>(offset);
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
    sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());

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

} // namespace

ImuBno055Manager::ImuBno055Manager(uart_comm::UartComm &uart, const ManagerConfig &config)
    : m_uart(uart),
      m_config(config),
      m_mutex(xSemaphoreCreateMutex()),
      m_config_queue(nullptr),
      m_valid(false),
      m_stream_state(),
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

    m_stream_state.enabled = m_config.stream_task.enable;
    m_stream_state.period_ms = std::clamp<uint16_t>(
        m_config.stream_task.period_ms,
        m_config.min_stream_period_ms,
        m_config.max_stream_period_ms);
    m_stream_state.field_mask = m_config.default_stream_field_mask;
    m_stream_state.operation_mode = m_config.default_operation_mode;

    if (!isValidOperationMode(m_stream_state.operation_mode)) {
        ESP_LOGE(TAG, "Invalid default operation mode: 0x%02X", m_stream_state.operation_mode);
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
        if (m_device->setOperationMode(m_stream_state.operation_mode) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set default operation mode: 0x%02X", m_stream_state.operation_mode);
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
    }

    m_valid.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "IMU manager ready (cmd=0x%02X rsp=0x%02X stream=0x%02X period=%u mask=0x%04X source=%s)",
             m_config.configure_msg_type,
             m_config.configure_response_msg_type,
             m_config.telemetry_stream_channel,
             m_stream_state.period_ms,
             m_stream_state.field_mask,
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

ResultCode ImuBno055Manager::setStreamEnabled(bool enabled)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }

    m_stream_state.enabled = enabled;
    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode ImuBno055Manager::setStreamPeriodMs(uint16_t period_ms)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }

    m_stream_state.period_ms = std::clamp<uint16_t>(
        period_ms,
        m_config.min_stream_period_ms,
        m_config.max_stream_period_ms);
    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode ImuBno055Manager::setStreamFieldMask(uint16_t field_mask)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }

    m_stream_state.field_mask = field_mask;
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
        m_stream_state.operation_mode = operation_mode;
        xSemaphoreGive(m_mutex);
        return ResultCode::Ok;
    }

    bool resume_stream = false;
    if (xSemaphoreTake(m_mutex, kMutexTimeout) != pdTRUE) {
        return ResultCode::Busy;
    }

    resume_stream = m_stream_state.enabled;
    m_stream_state.enabled = false;
    xSemaphoreGive(m_mutex);

    const esp_err_t ret = m_device->setOperationMode(operation_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set operation mode 0x%02X: %s",
                 operation_mode,
                 esp_err_to_name(ret));
        if (xSemaphoreTake(m_mutex, kMutexTimeout) == pdTRUE) {
            m_stream_state.enabled = resume_stream;
            xSemaphoreGive(m_mutex);
        }
        return ResultCode::InternalError;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeout) == pdTRUE) {
        m_stream_state.operation_mode = operation_mode;
        m_stream_state.enabled = resume_stream;
        xSemaphoreGive(m_mutex);
    }

    return ResultCode::Ok;
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
        return ResultCode::InternalError;
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
        return ResultCode::InternalError;
    }
    return ResultCode::Ok;
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
    // body is the payload after the configure request header; len is its length.
    response_len = 0;

    switch (subcommand) {
        case ConfigureSubcommand::SetStreamEnabled: {
            if (len < 1U) {
                return ResultCode::InvalidArgument;
            }
            return setStreamEnabled(body[0] != 0U);
        }
        case ConfigureSubcommand::SetStreamPeriodMs: {
            if (len < 2U) {
                return ResultCode::InvalidArgument;
            }
            size_t offset = 0U;
            bool ok = true;
            const uint16_t period_ms = readU16Le(body, len, offset, ok);
            if (!ok) {
                return ResultCode::InvalidArgument;
            }
            return setStreamPeriodMs(period_ms);
        }
        case ConfigureSubcommand::SetStreamFieldMask: {
            if (len < 2U) {
                return ResultCode::InvalidArgument;
            }
            size_t offset = 0U;
            bool ok = true;
            const uint16_t mask = readU16Le(body, len, offset, ok);
            if (!ok) {
                return ResultCode::InvalidArgument;
            }
            return setStreamFieldMask(mask);
        }
        case ConfigureSubcommand::SetOperationMode: {
            if (len < 1U) {
                return ResultCode::InvalidArgument;
            }
            return setOperationMode(body[0]);
        }
        case ConfigureSubcommand::ReadCalibration: {
            CalibrationBlob blob{};
            const ResultCode rc = readCalibration(blob);
            if (rc != ResultCode::Ok) {
                return rc;
            }
            if (response_capacity < kCalibrationPayloadSize) {
                return ResultCode::InternalError;
            }
            encodeCalibrationBlob(blob, response_payload, response_capacity, response_len);
            return ResultCode::Ok;
        }
        case ConfigureSubcommand::WriteCalibration: {
            size_t offset = 0U;
            CalibrationBlob blob{};
            if (!decodeCalibrationBlob(body, len, offset, blob)) {
                return ResultCode::InvalidArgument;
            }
            return writeCalibration(blob);
        }
        case ConfigureSubcommand::ApplyCalibration:
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
    if (extra_len > kMaxResponseExtraPayload) {
        return ESP_ERR_INVALID_ARG;
    }

    payload[0] = kProtocolVersion;
    payload[1] = seq;
    payload[2] = subcommand;
    payload[3] = static_cast<uint8_t>(code);
    if (extra_len > 0U && extra_payload != nullptr) {
        std::memcpy(&payload[kConfigureResponseHeaderSize], extra_payload, extra_len);
    }

    return m_uart.sendCommand(m_config.configure_response_msg_type, payload,
                              static_cast<uint8_t>(kConfigureResponseHeaderSize + extra_len));
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

    // Join: wait until both task bodies have returned. Neither is ever
    // deleted while it holds m_mutex or is inside an I2C transaction.
    m_stream_task.stop();
    m_config_task.stop();

    m_valid.store(false, std::memory_order_release);
}

void ImuBno055Manager::streamTaskLoop()
{
    ImuBno055Manager *const self = this;
    TickType_t last_wake = xTaskGetTickCount();

    while (!m_stream_task.stopRequested()) {
        StreamState state{};
        if (xSemaphoreTake(self->m_mutex, kMutexTimeout) == pdTRUE) {
            state = self->m_stream_state;
            xSemaphoreGive(self->m_mutex);
        }

        if (state.enabled) {
            ImuStreamSample sample{};
            esp_err_t read_ret = ESP_ERR_INVALID_STATE;
            if (self->m_use_mock_data) {
                populateMockSample(state.field_mask, sample);
                read_ret = ESP_OK;
            } else if (self->m_device != nullptr && self->m_device->isValid()) {
                read_ret = self->m_device->readSample(state.field_mask, sample);
            }

            if (read_ret == ESP_OK) {
                uint8_t tx_payload[kMaxStreamPayload] = {0};
                uint8_t tx_len = 0;
                if (encodeStreamSample(sample, state.field_mask, tx_payload, sizeof(tx_payload), tx_len)) {
                    self->m_uart.sendData(self->m_config.telemetry_stream_channel, tx_payload, tx_len);
                } else {
                    ESP_LOGW(TAG, "Stream encode overflow mask=0x%04X", state.field_mask);
                }
            } else {
                ESP_LOGW(TAG, "Sensor read failed: %s", esp_err_to_name(read_ret));
            }
        }

        const TickType_t period = std::max<TickType_t>(1, pdMS_TO_TICKS(state.period_ms));
        m_stream_task.sleepUntil(last_wake, period);
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
