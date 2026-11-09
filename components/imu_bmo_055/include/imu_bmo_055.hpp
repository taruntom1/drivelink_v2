#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu_bmo_055_types.hpp"
#include "uart_comm.hpp"
#include "stoppable_task.hpp"
#include "task_timer.hpp"

#include <atomic>
#include <memory>

namespace imu_bmo_055 {

class Bno055Device;

class ImuBno055Manager {
public:
	explicit ImuBno055Manager(uart_comm::UartComm &uart,
							  const ManagerConfig &config = ManagerConfig{});
	~ImuBno055Manager();

	ImuBno055Manager(const ImuBno055Manager &) = delete;
	ImuBno055Manager &operator=(const ImuBno055Manager &) = delete;
	ImuBno055Manager(ImuBno055Manager &&) = delete;
	ImuBno055Manager &operator=(ImuBno055Manager &&) = delete;

	bool isValid() const;

	/// Start or stop streaming. Stopping also stops the sampling timer.
	ResultCode setStreamEnabled(bool enabled);
	/// Set the sample (timer) period and the telemetry (packet) period, in us.
	/// 0 keeps the current value. The sample period is clamped to
	/// [min, max]_sample_period_us; the telemetry period to
	/// [sample period, max_sample_period_us], rounded up to a whole multiple
	/// of the sample period. The applied values are returned.
	ResultCode setStreamTiming(uint32_t sample_period_us,
							   uint32_t telemetry_period_us,
							   uint32_t &applied_sample_period_us,
							   uint32_t &applied_telemetry_period_us);
	/// Rejects bits outside kStreamFieldAll with InvalidArgument.
	ResultCode setStreamFieldMask(uint16_t field_mask);
	ResultCode setOperationMode(uint8_t operation_mode);
	ResultCode readCalibration(CalibrationBlob &out);
	ResultCode writeCalibration(const CalibrationBlob &blob);
	ResultCode applyCalibration();

	/// Stream health counters (running totals). Lock-free.
	StreamCounters getStreamCounters() const;

	void handleConfigureCommand(const uart_comm::RxFrame &frame);

private:
	void streamTaskLoop();
	void configTaskLoop();

	/// Set the enabled flag and start/stop the timer. Caller holds m_mutex.
	ResultCode applyStreamEnabledLocked(bool enabled);

	ResultCode parseAndDispatchConfigure(ConfigureSubcommand subcommand,
										 const uint8_t *body, uint8_t len,
										 uint8_t &response_len, uint8_t *response_payload,
										 size_t response_capacity);
	esp_err_t sendConfigureResponse(uint8_t seq,
									uint8_t subcommand,
									ResultCode code,
									const uint8_t *extra_payload,
									uint8_t extra_len);
	void stopTasks();

	uart_comm::UartComm &m_uart;
	ManagerConfig m_config;
	mutable SemaphoreHandle_t m_mutex;
	QueueHandle_t m_config_queue;
	rtos_util::StoppableTask m_stream_task;
	rtos_util::StoppableTask m_config_task;
	// Declared after the task it wakes, so it is destroyed first.
	rtos_util::TaskTimer m_stream_timer; // calls guarded by m_mutex
	std::atomic<bool> m_valid;

	// Stream state. Written under m_mutex by the config path, read lock-free
	// by the stream task.
	std::atomic<bool> m_stream_enabled;
	std::atomic<uint16_t> m_field_mask;
	std::atomic<uint32_t> m_sample_period_us;
	std::atomic<uint32_t> m_telemetry_divider;
	/// Set on (re-)enable so the stream task publishes its first sample and
	/// does not count the idle time as late ticks.
	std::atomic<bool> m_stream_restarted;
	uint32_t m_requested_telemetry_period_us; // guarded by m_mutex
	uint8_t m_operation_mode;                 // guarded by m_mutex

	std::atomic<uint32_t> m_late_count;
	std::atomic<uint32_t> m_busy_count;
	std::atomic<uint32_t> m_read_error_count;
	std::atomic<uint32_t> m_encode_error_count;

	CalibrationBlob m_cached_calibration;
	bool m_has_cached_calibration;
	bool m_use_mock_data;
	std::unique_ptr<Bno055Device> m_device;
	uart_comm::CallbackRegistration m_configure_cb;
};

} // namespace imu_bmo_055
