#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "system_health.h"
#include "plug_manager.h"
#include "net_scan.h"
#include "wifi_manager.h"
#include "event_log.h"

static const char *TAG = "system_health";

/* No HTTP calls, no big buffers -- just a handful of small checks and one
 * short snprintf(), a default-sized task stack would already be overkill;
 * 4096 is generous headroom over what this actually needs. */
#define HEALTH_TASK_STACK        4096
/* Comfortably under CONFIG_ESP_TASK_WDT_TIMEOUT_S (5s, see sdkconfig) --
 * this task must call esp_task_wdt_reset() more often than that timeout or
 * it triggers the watchdog on itself. */
#define HEALTH_LOOP_INTERVAL_MS  2000

/* Both plug_manager's and net_scan's locks are only ever held for a brief
 * snapshot/update, never across a network call (see the discipline
 * documented throughout both files) -- 2s is already a very generous
 * bounded wait for a lock that should free up in low-single-digit
 * milliseconds under normal operation. Checked every 5 minutes; two
 * consecutive failures (~5-10 minutes of confirmed trouble) before
 * treating it as a real deadlock and restarting, so one unlucky scheduling
 * hiccup doesn't cause an unnecessary reboot on a device meant to
 * prioritize uptime. */
#define LOCK_PROBE_INTERVAL_MS       (5 * 60 * 1000)
#define LOCK_PROBE_TIMEOUT_MS        2000
#define LOCK_PROBE_FAIL_STREAK_LIMIT 2

/* wifi_manager.c already retries STA connection attempts indefinitely on
 * its own for any "this might still work" disconnect reason (see its
 * s_sta_gave_up logic) -- this is a last-resort escape hatch for the case
 * where that retry loop itself is running but never actually succeeding
 * for an unusually long time, which can happen if the Wi-Fi driver gets
 * into a state a plain esp_wifi_connect() retry can't clear on its own.
 * Does NOT fire if wifi_manager_sta_config_error() is true (wrong
 * password/SSID not found) -- restarting endlessly would never fix that,
 * only the user changing the credentials in the UI can. */
#define WIFI_STUCK_RESTART_MS (15 * 60 * 1000)

/* Forensic trail for whenever the *next* hang happens -- ESP_LOGI'd only
 * (see health_task() below), not written to the persisted event log. */
#define HEARTBEAT_INTERVAL_MS (30 * 60 * 1000)

static void restart_with_log(const char *reason)
{
    ESP_LOGE(TAG, "restarting: %s", reason);
    event_log_append(LOG_SRC_SYSTEM, "-", "self_restart", false, reason);
    /* Give the log write (LittleFS) and the ESP_LOGE line above a moment to
     * actually land before the reset -- best-effort, not a guarantee. */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void health_task(void *arg)
{
    (void)arg;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        /* Not fatal -- the rest of this task's job (lock probes, Wi-Fi
         * stuck check, heartbeat) is still useful on its own even without
         * TWDT coverage of this particular task. */
        ESP_LOGW(TAG, "could not subscribe to task watchdog: %s", esp_err_to_name(wdt_err));
    }

    int64_t now = esp_timer_get_time();
    int64_t next_lock_probe_us = now;
    int64_t next_heartbeat_us = now;
    int64_t wifi_disconnected_since_us = 0; /* 0 == currently connected */
    int lock_probe_fail_streak = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_LOOP_INTERVAL_MS));
        esp_task_wdt_reset();

        now = esp_timer_get_time();

        /* --- deadlock probe: plug_manager's and net_scan's locks --- */
        if (now >= next_lock_probe_us) {
            next_lock_probe_us = now + (int64_t)LOCK_PROBE_INTERVAL_MS * 1000;

            bool ok = plug_manager_is_responsive(LOCK_PROBE_TIMEOUT_MS) &&
                      net_scan_is_responsive(LOCK_PROBE_TIMEOUT_MS);
            if (ok) {
                lock_probe_fail_streak = 0;
            } else {
                lock_probe_fail_streak++;
                ESP_LOGW(TAG, "lock probe failed (streak=%d/%d) -- plug_manager or net_scan's "
                         "lock did not free up within %dms",
                         lock_probe_fail_streak, LOCK_PROBE_FAIL_STREAK_LIMIT, LOCK_PROBE_TIMEOUT_MS);
                if (lock_probe_fail_streak >= LOCK_PROBE_FAIL_STREAK_LIMIT) {
                    restart_with_log("plug_manager/net_scan lock stuck (probable deadlock)");
                }
            }
        }

        /* --- Wi-Fi stuck check (reuses the same wake cycle, cheap) ---
         * Only meaningful if STA credentials are actually configured --
         * pure AP-only usage (a deliberately supported mode, see README
         * "Wi-Fi") never even attempts a STA connection, so
         * wifi_manager_is_connected() would stay false forever and this
         * would otherwise restart the device every 15 minutes, endlessly,
         * for a perfectly healthy setup. Empty ssid == no STA configured. */
        char ssid[33];
        wifi_manager_get_status(NULL, 0, NULL, 0, ssid, sizeof(ssid));
        bool sta_configured = (ssid[0] != '\0');

        if (sta_configured && !wifi_manager_is_connected()) {
            if (wifi_disconnected_since_us == 0) {
                wifi_disconnected_since_us = now;
            } else if (!wifi_manager_sta_config_error() &&
                       (now - wifi_disconnected_since_us) > (int64_t)WIFI_STUCK_RESTART_MS * 1000) {
                restart_with_log("STA disconnected far longer than expected despite auto-retry");
            }
        } else {
            wifi_disconnected_since_us = 0;
        }

        /* --- periodic heartbeat --- */
        if (now >= next_heartbeat_us) {
            next_heartbeat_us = now + (int64_t)HEARTBEAT_INTERVAL_MS * 1000;
            /* Internal free heap alone wasn't enough to diagnose a
             * "gets slow after long uptime" report -- it turned out to sit
             * in a flat, tight band the whole time, which just reflects
             * this project's static buffers (CONFIG_SPIRAM_MALLOC_
             * ALWAYSINTERNAL keeps big one-off allocations like
             * h_plugs_get()'s off of it and onto PSRAM instead). Logging
             * PSRAM's free size and largest free block too closes that
             * blind spot -- a slow *growing* gap between "free" and
             * "largest block" on either heap is the actual fragmentation
             * signal, not the free total by itself. */
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "uptime=%llds heap=%uB heap_blk=%uB psram=%uB psram_blk=%uB sta=%s",
                     (long long)(now / 1000000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                     wifi_manager_is_connected() ? "up" : "down");
            /* ESP_LOGI only, not event_log_append() -- the persisted event
             * log now caps at EVENT_LOG_MAX_LINES (50) most-recent entries
             * (see event_log.c), and a heartbeat every 30 minutes would
             * flood that out within ~25 hours, burying real plug/input/
             * API events under nothing but heartbeats. Serial/UART is
             * still the place to capture this for a heap-fragmentation
             * hunt (idf.py monitor, or a serial logger left running
             * overnight), same as before. */
            ESP_LOGI(TAG, "heartbeat: %s", detail);
        }
    }
}

esp_err_t system_health_init(void)
{
    BaseType_t ok = xTaskCreate(health_task, "sys_health", HEALTH_TASK_STACK, NULL, 2, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
