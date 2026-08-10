#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Background reliability watchdog for a device meant to run unattended for
 * a long time (see README "Надійність / вихід із зависань" for the full
 * rationale). Three separate, independent things, all handled by one small
 * task:
 *
 * 1. Subscribes itself to the ESP-IDF Task Watchdog Timer (TWDT) and pets
 *    it every loop iteration. Combined with CONFIG_ESP_TASK_WDT_PANIC=y
 *    (see sdkconfig.defaults), a task that hogs a CPU core long enough to
 *    starve the idle task now actually reboots the device instead of just
 *    logging a warning and carrying on stuck.
 *
 * 2. Periodically probes plug_manager's and net_scan's internal locks with
 *    a *bounded* wait (plug_manager_is_responsive() / net_scan_is_
 *    responsive()) -- unlike the TWDT above, this specifically catches a
 *    deadlock (two tasks stuck waiting on each other's mutex forever),
 *    which the TWDT cannot see at all: a deadlocked system's CPU is idle,
 *    not hogged, so the idle-task-based watchdog never notices. Two
 *    consecutive failed probes (a few minutes apart) are treated as a
 *    confirmed deadlock and trigger a deliberate esp_restart() -- a single
 *    failed probe is not enough on its own, to avoid restarting over a
 *    one-off scheduling hiccup rather than a real stuck lock.
 *
 * 3. If STA has credentials configured, hasn't given up in the "wrong
 *    password" sense (wifi_manager_sta_config_error()), and still hasn't
 *    reconnected after an unusually long stretch despite wifi_manager's
 *    own unlimited-retry logic (see wifi_manager.c) -- restarts as a last
 *    resort. Some Wi-Fi driver states only clear on a full reinit, not a
 *    plain reconnect attempt.
 *
 * Also logs a periodic heartbeat (uptime, free internal SRAM, STA state)
 * to the event log, so a hang that *does* still happen eventually leaves a
 * forensic trail on flash even without a live serial monitor attached. */
esp_err_t system_health_init(void);

#ifdef __cplusplus
}
#endif
