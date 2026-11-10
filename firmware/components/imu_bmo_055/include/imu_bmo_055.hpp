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

	// Each command returns its result code and why it failed (ResultDetail),
	// as sent in the configure response.

	/// Start or stop streaming. Stopping also stops the sampling timer.
	Result setStreamEnabled(bool enabled);
	/// Set the sample (timer) period and the telemetry (packet) period, in us.
	/// 0 keeps the current value. The sample period is clamped to
	/// [min, max]_sample_period_us; the telemetry period to
	/// [sample period, max_sample_period_us], rounded up to a whole multiple
	/// of the sample period. The applied values are returned.
	Result setStreamTiming(uint32_t sample_period_us,
						   uint32_t telemetry_period_us,
						   uint32_t &applied_sample_period_us,
						   uint32_t &applied_telemetry_period_us);
	/// Rejects bits outside kStreamFieldAll with InvalidArgument.
	Result setStreamFieldMask(uint16_t field_mask);
	Result setOperationMode(uint8_t operation_mode);
	Result readCalibration(CalibrationBlob &out);
	Result writeCalibration(const CalibrationBlob &blob);
	Result applyCalibration();

	void handleConfigureCommand(const uart_comm::RxFrame &frame);

private:
	void streamTaskLoop();
	void configTaskLoop();

	/// Set the enabled flag and start/stop the timer. Caller holds m_mutex.
	/// Returns the timer error, if any.
	esp_err_t applyStreamEnabledLocked(bool enabled);

	Result parseAndDispatchConfigure(ConfigureSubcommand subcommand,
									 const uint8_t *body, uint8_t len,
									 uint8_t &response_len, uint8_t *response_payload,
									 size_t response_capacity);
	/// Encode and queue a configure response. Non-blocking, so the RX task
	/// can use it too.
	esp_err_t sendConfigureResponse(uint8_t seq,
									uint8_t subcommand,
									Result result,
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

	/// Bumped whenever the sampling timer is (re)started or re-timed. A wait
	/// that times out across a bump is not a stall.
	std::atomic<uint32_t> m_timer_epoch{0};

	CalibrationBlob m_cached_calibration;
	bool m_has_cached_calibration;
	bool m_use_mock_data;
	std::unique_ptr<Bno055Device> m_device;
	uart_comm::CallbackRegistration m_configure_cb;
};

} // namespace imu_bmo_055
