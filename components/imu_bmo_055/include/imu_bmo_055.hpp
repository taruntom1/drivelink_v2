#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu_bmo_055_types.hpp"
#include "uart_comm.hpp"

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

	ResultCode setStreamEnabled(bool enabled);
	ResultCode setStreamPeriodMs(uint16_t period_ms);
	ResultCode setStreamFieldMask(uint16_t field_mask);
	ResultCode setOperationMode(uint8_t operation_mode);
	ResultCode readCalibration(CalibrationBlob &out);
	ResultCode writeCalibration(const CalibrationBlob &blob);
	ResultCode applyCalibration();

	void handleConfigureCommand(const uart_comm::RxFrame &frame);

private:
	struct StreamState {
		bool enabled = true;
		uint16_t period_ms = 10;
		uint16_t field_mask = kStreamFieldAll;
		uint8_t operation_mode = 0x0C;
	};

	static void streamTaskEntry(void *arg);
	static void configTaskEntry(void *arg);

	ResultCode parseAndDispatchConfigure(const uint8_t *payload, uint8_t len,
										 ConfigureSubcommand &subcommand,
										 uint8_t &response_len, uint8_t *response_payload,
										 size_t response_capacity);
	esp_err_t sendConfigureResponse(ConfigureSubcommand subcommand,
									ResultCode code,
									const uint8_t *extra_payload,
									uint8_t extra_len);
	void stopTasks();

	uart_comm::UartComm &m_uart;
	ManagerConfig m_config;
	mutable SemaphoreHandle_t m_mutex;
	QueueHandle_t m_config_queue;
	TaskHandle_t m_stream_task;
	TaskHandle_t m_config_task;
	std::atomic<bool> m_valid;
	std::atomic<bool> m_stop_stream_task;
	std::atomic<bool> m_stop_config_task;
	StreamState m_stream_state;
	CalibrationBlob m_cached_calibration;
	bool m_has_cached_calibration;
	bool m_use_mock_data;
	std::unique_ptr<Bno055Device> m_device;
};

} // namespace imu_bmo_055

