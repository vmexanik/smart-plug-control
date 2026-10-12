#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

/* Defined by main/Kconfig.projbuild (default 24). The fallback keeps the build
 * working when an existing sdkconfig has not been regenerated yet
 * (`idf.py reconfigure` / menuconfig adds the real symbol). */
#ifndef CONFIG_NTP_SYNC_INTERVAL_HOURS
#define CONFIG_NTP_SYNC_INTERVAL_HOURS 24
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the SNTP client against ntp_server (e.g. "pool.ntp.org"). Safe to
 * call with an empty/NULL server -- NTP sync is simply not started, but
 * time_sync_set_manual() still works. Actual sync only happens once Wi-Fi
 * STA has an IP; this just arms it (non-blocking). */
esp_err_t time_sync_init(const char *ntp_server);

/* True once system time has been set, either by a completed NTP sync or by
 * time_sync_set_manual(). False means the clock is still at the epoch
 * (1970) default. */
bool time_sync_is_synced(void);

/* "ntp" if the last successful sync was via SNTP, "manual" if set by
 * time_sync_set_manual(), "none" if never synced. */
const char *time_sync_source(void);

/* Unix time of the last completed NTP sync, 0 if there has been none (e.g.
 * the clock was only ever set manually). Together with
 * CONFIG_NTP_SYNC_INTERVAL_HOURS this tells when the next periodic
 * re-sync is due. */
int64_t time_sync_last_ntp_epoch(void);

/* Directly sets the system clock (e.g. from a value entered in the web UI
 * when there's no internet access for NTP). */
esp_err_t time_sync_set_manual(int64_t epoch_seconds);

#ifdef __cplusplus
}
#endif
