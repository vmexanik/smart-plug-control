#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

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

/* Fills pass_out with the currently stored STA password (empty string if
 * none configured yet). The web UI's settings form needs the real value to
 * prefill the password field -- otherwise clicking "Підключити" without
 * retyping the password resubmits an empty one and overwrites the working
 * credential. This device has no auth on its own web UI to begin with
 * (see README), so echoing the password back to the same LAN client that
 * could already read/change it isn't a new exposure. */
void wifi_manager_get_sta_password(char *pass_out, size_t pass_size);

/* True once STA has failed to associate 3 times in a row for a reason that
 * retrying won't fix on its own (wrong password, SSID not found, ...) --
 * see wifi_manager.c's is_config_error_reason() for the exact reason codes.
 * Automatic reconnection stops once this is true, so the device doesn't
 * spam association attempts against an AP that keeps rejecting it; it
 * resets on the next wifi_manager_set_credentials() call or reboot. Purely
 * a status flag for the UI -- doesn't affect the AP fallback, which is
 * always up. */
bool wifi_manager_sta_config_error(void);

#define WIFI_SCAN_MAX_RESULTS 20

typedef struct {
    char ssid[33];
    bool secure;   /* false = open network, no password needed */
} wifi_scan_result_t;

/* Starts an async scan for nearby Wi-Fi networks in a dedicated task (poll
 * via wifi_manager_get_scan_status() / wifi_manager_get_scan_result() --
 * the "async" here is from the HTTP handler's point of view; the scan
 * itself briefly blocks whichever task runs it). Results are de-duplicated
 * by SSID -- a mesh/multi-AP network broadcasts the same SSID from several
 * BSSIDs, and esp_wifi already returns the raw list sorted by RSSI
 * descending, so keeping the first occurrence of each SSID is enough to
 * keep the strongest one. Capped at WIFI_SCAN_MAX_RESULTS; hidden networks
 * (empty SSID) are skipped since there'd be nothing to show or pick.
 *
 * Per Espressif's own docs the default per-channel scan time (~120ms) is
 * well under the 1500ms they warn can drop an active STA association, so
 * this is safe to run even while already connected -- expect a brief
 * hiccup in STA throughput while it runs, not a disconnect.
 *
 * Returns:
 *   ESP_OK                  scan started
 *   ESP_ERR_INVALID_STATE   a scan is already running
 *   ESP_ERR_NO_MEM           couldn't create the scan task
 */
esp_err_t wifi_manager_scan_start(void);

void wifi_manager_get_scan_status(bool *running, size_t *count);

/* Copies results[index] into *out. Returns false (*out left untouched) if
 * index >= the count reported by wifi_manager_get_scan_status(). */
bool wifi_manager_get_scan_result(size_t index, wifi_scan_result_t *out);

/* Fills *out with the STA interface's current IP/netmask/gw. Returns false
 * (out left untouched) if STA is not connected -- used by net_scan.c to
 * compute which subnet to sweep. */
bool wifi_manager_get_sta_ip_info(esp_netif_ip_info_t *out);

/* Fills *out with the STA interface's configured main DNS server (normally
 * handed out by the router's DHCP lease). Returns false (out left
 * untouched) if STA is not connected or no IPv4 DNS server is configured --
 * used by net_scan.c for best-effort reverse-DNS hostname lookups. */
bool wifi_manager_get_sta_dns(esp_ip4_addr_t *out);

#ifdef __cplusplus
}
#endif
