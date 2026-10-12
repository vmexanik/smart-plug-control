#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Split into two steps (netif/event-loop plumbing here, the actual Wi-Fi
 * driver bring-up in wifi_manager_start() below) instead of one
 * wifi_manager_init(), specifically so app_main() can create plug_manager's
 * (and gpio_inputs'/ac_monitor's) background tasks -- fixed-size stacks
 * totalling >20KB -- in between the two. esp_wifi_init()/esp_wifi_start()
 * (in wifi_manager_start()) allocate the Wi-Fi driver's static+dynamic
 * RX/TX buffers, which on this chip's internal DRAM (a scarce, non-PSRAM
 * resource shared with everything else -- see external-RAM docs: task
 * stacks can't simply move to PSRAM without ROM-call caveats) easily runs
 * to 60-90KB in combined AP+STA mode. Doing that *before* plug_manager's
 * task creation left too little internal DRAM free and plug_manager_init()
 * failed with ESP_ERR_NO_MEM on real hardware. This function only sets up
 * esp_netif_init()/the default event loop/the AP+STA esp_netif objects --
 * cheap, and needed early because esp-mqtt (driver_mqtt.c, invoked from
 * inside plug_manager_init()) requires lwIP/the event loop to already
 * exist if a broker URI is configured, even though actually connecting
 * doesn't require Wi-Fi to be started yet (mqtt retries on its own). */
esp_err_t wifi_manager_init_netif(void);

/*
 * Always runs Wi-Fi in APSTA mode:
 *  - AP "SmartPlug-XXXXXX" (password: the one saved in NVS via
 *    wifi_manager_set_ap_password(), else Kconfig AP_FALLBACK_PASSWORD) is
 *    always on at 192.168.4.1, so the web UI is reachable even if STA
 *    credentials are missing/wrong.
 *  - STA connects using credentials stored in NVS (namespace "wifi"), if any.
 *
 * Must be called after wifi_manager_init_netif(). This is the memory-heavy
 * half (see wifi_manager_init_netif()'s doc comment) -- call it only once
 * the other early, internal-DRAM-hungry tasks (plug_manager/gpio_inputs/
 * ac_monitor) already exist. */
esp_err_t wifi_manager_start(void);

/* Persists new STA credentials to NVS and (re)connects without reboot. */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/* Saves a new soft-AP password to NVS (namespace "wifi", key "ap_pass").
 * Must be a valid WPA2 passphrase: 8..63 printable ASCII characters,
 * otherwise ESP_ERR_INVALID_ARG (an open AP is deliberately not selectable
 * at runtime). Does NOT touch the running AP -- call
 * wifi_manager_apply_ap_password() for that, ideally after the HTTP
 * response has been sent, since reconfiguring the AP kicks every station
 * (including the browser that asked for the change if it is on the AP). */
esp_err_t wifi_manager_set_ap_password(const char *password);

/* Pushes the password saved by wifi_manager_set_ap_password() into the
 * running soft-AP (esp_wifi_set_config(WIFI_IF_AP)); stations must
 * reconnect with the new password. */
esp_err_t wifi_manager_apply_ap_password(void);

/* Current soft-AP password (saved one, or the Kconfig default). Like the
 * STA password above, it is echoed to the web UI (which has no auth). */
void wifi_manager_get_ap_password(char *pass_out, size_t pass_size);

/* Erases the STA credentials (SSID+password) AND the custom AP password
 * persisted in NVS (namespace "wifi") and clears the in-memory copies,
 * reverting the device to "no STA configured, AP-only, default AP
 * password" -- the same state as first boot. (The AP password returns to
 * the Kconfig default in the running AP only after the next boot.) Callers
 * that don't immediately reboot will see wifi_manager_get_status()'s
 * ssid_out go back to empty right away; esp_wifi_disconnect()/connect()
 * are deliberately NOT called here (unlike wifi_manager_set_credentials()),
 * since the one caller today (POST /api/system/factory_reset in
 * web_server.c) reboots the device immediately after. */
esp_err_t wifi_manager_erase_credentials(void);

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
 * retrying won't fix on its own -- either a real auth/handshake failure, or
 * (only if these credentials have never once actually connected) "AP not
 * found". A previously-working AP that temporarily disappears does NOT set
 * this -- it retries forever instead, same as any normal device reconnecting
 * after its router reboots. See wifi_manager.c's is_hard_auth_failure_reason()
 * / is_ap_not_found_reason() / s_sta_ever_connected for the exact logic.
 * Automatic reconnection stops once this is true, so the device doesn't
 * spam association attempts against an AP that keeps rejecting it; it
 * resets on the next wifi_manager_set_credentials() call or reboot. Purely
 * a status flag for the UI -- doesn't affect the AP fallback, which is
 * always up. */
bool wifi_manager_sta_config_error(void);

/* Fills *out with the current STA link's RSSI (dBm, negative -- e.g. -55 is
 * a strong signal, -85 is barely hanging on) via esp_wifi_sta_get_ap_info().
 * Returns false (out left untouched) if not currently connected -- same
 * "false means don't trust *out" convention as wifi_manager_get_sta_ip_info()
 * / wifi_manager_get_sta_dns() above. For the web UI's signal-bars icon
 * (GET /api/status -> wifi.rssi). */
bool wifi_manager_get_sta_rssi(int8_t *out);

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

/* Fills *out with the AP interface's IP/netmask/gw (always 192.168.4.1/24
 * while the fallback AP is up, which per WIFI_MODE_APSTA is always -- see
 * wifi_manager_get_status()'s doc comment). Returns false (out left
 * untouched) only if the AP netif itself somehow isn't ready yet. Used by
 * captive_dns.c to know which address to answer every DNS query with. */
bool wifi_manager_get_ap_ip_info(esp_netif_ip_info_t *out);

/* SSID of the soft-AP as currently configured (e.g. "SmartPlug-A1B2C3").
 * Writes an empty string on failure. */
void wifi_manager_get_ap_ssid(char *out, size_t out_size);

/* Fills *out with the STA interface's configured main DNS server (normally
 * handed out by the router's DHCP lease). Returns false (out left
 * untouched) if STA is not connected or no IPv4 DNS server is configured --
 * used by net_scan.c for best-effort reverse-DNS hostname lookups. */
bool wifi_manager_get_sta_dns(esp_ip4_addr_t *out);

#ifdef __cplusplus
}
#endif
