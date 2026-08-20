#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* source strings used in log entries */
#define LOG_SRC_INPUT   "input"
#define LOG_SRC_MANUAL  "manual"
#define LOG_SRC_API     "api"
#define LOG_SRC_BOOT    "boot"
#define LOG_SRC_SYSTEM  "system"
#define LOG_SRC_AC_MON  "ac_mon"

esp_err_t event_log_init(const system_settings_t *settings);

/* Call after system settings (log_max_file_size / log_max_files) change. */
void event_log_update_limits(uint32_t max_file_size, uint32_t max_files);

/* Appends one ND-JSON event line, rotating the log file if it grew past the
 * configured size. detail may be NULL. Safe to call from any task. */
void event_log_append(const char *source, const char *plug_name,
                       const char *action, bool ok, const char *detail);

/* Fills out_json (a JSON array string) with up to max_lines most recent
 * entries, newest last. Returns ESP_OK, or ESP_ERR_NO_MEM if it doesn't fit
 * in out_size. */
esp_err_t event_log_read_recent(char *out_json, size_t out_size, int max_lines);

/* Deletes the active log and all rotated files. */
esp_err_t event_log_clear(void);

#ifdef __cplusplus
}
#endif
