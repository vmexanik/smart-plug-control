#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Always runs Wi-Fi in APSTA mode:
 *  - AP "SmartPlug-XXXXXX" (password from Kconfig AP_FALLBACK_PASSWORD) is
 *    always on at 192.168.4.1, so the web UI is reachable even if STA
 *    credentials are missing/wrong.
 *  - STA connects using credentials stored in NVS (namespace "wifi"), if any.
 */
esp_err_t wifi_manager_init(void);

/* Persists new STA credentials to NVS and (re)connects without reboot. */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

bool wifi_manager_is_connected(void);

/* Fills sta_ip_out (dotted quad, or "0.0.0.0" if not connected) and
 * ap_ip_out (always 192.168.4.1 while AP is up). ssid_out receives the
 * currently configured STA SSID (empty string if none). */
void wifi_manager_get_status(char *sta_ip_out, size_t sta_ip_size,
                              char *ap_ip_out, size_t ap_ip_size,
                              char *ssid_out, size_t ssid_size);

#ifdef __cplusplus
}
#endif
