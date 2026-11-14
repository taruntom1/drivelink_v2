#pragma once

// Host-test stand-in for ESP-IDF's esp_log.h. Log calls are type-checked
// against their format string (like on the target) and then dropped.

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static inline void host_log_discard(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

#define ESP_LOGE(tag, fmt, ...) host_log_discard(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) host_log_discard(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) host_log_discard(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) host_log_discard(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) host_log_discard(tag, fmt, ##__VA_ARGS__)
