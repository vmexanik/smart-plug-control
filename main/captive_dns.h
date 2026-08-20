#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts a minimal DNS server bound to the fallback AP's own IP (see
 * wifi_manager_get_ap_ip_info()) that answers EVERY query with that same
 * IP, regardless of the domain name asked for. Combined with
 * wifi_manager_init()'s AP DHCP DNS option (which points AP clients' DNS
 * queries at us in the first place) and web_server.c's OS-specific
 * captive-portal probe routes (/generate_204, /hotspot-detect.html, ...),
 * this is what makes a phone joining the fallback AP pop up a "Sign in to
 * network" prompt straight to this device's settings page, instead of the
 * user having to know to open a browser and type the AP's IP themselves.
 * Only ever touches traffic addressed to the AP's own IP -- never
 * interferes with the STA interface's normal DNS resolution (fingerprint.c,
 * NTP, reverse-DNS lookups in net_scan.c all keep working against whatever
 * DNS server the STA's own DHCP lease provides). Call once, any time after
 * wifi_manager_init(); safe to call even before the AP has a connected
 * client. */
esp_err_t captive_dns_init(void);

#ifdef __cplusplus
}
#endif
