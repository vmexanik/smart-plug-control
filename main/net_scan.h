#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Local-network device discovery.
 *
 * This is intentionally NOT "auto-configure my smart plug" in the general
 * case -- there's no universal control protocol across plug firmwares. What
 * this module does: ping-sweep the STA subnet, flag hosts with TCP:80 open,
 * then run fingerprint.c's Shelly/Tasmota/ESPHome detection cascade against
 * each of those. When the cascade recognizes a family, the result comes
 * back with an on/off URL template already filled in; when it doesn't, the
 * web UI still lets the user pick the bare IP and fill the form in by hand
 * (unchanged fallback behavior).
 *
 * Scope/limits (see README "Автопошук у мережі" for the full list,
 * including what was intentionally left out of the uploaded discovery
 * spec this was adapted from -- mDNS, raw-ARP/MAC-keyed caching, ESPHome's
 * native binary API, Sonoff DIY):
 *  - Only sweeps the STA interface's subnet (capped to a /24-sized window).
 *    Devices on the fallback AP's 192.168.4.0/24, or on a different VLAN/
 *    subnet than the hub, will not be found.
 *  - A "responds to ping" host is not necessarily a smart plug -- phones,
 *    laptops, routers etc. will show up too; fingerprinting narrows that
 *    down further but "family unknown" hosts are still listed as bare IPs.
 *  - Fingerprinting assumes a single/first channel (Shelly id=0, Tasmota
 *    relay 1) -- multi-channel strips are detected as PLUG_KIND_RELAY and
 *    the URL template still points at the first channel; adjust manually
 *    in the form for channel 2+.
 */

#define NET_SCAN_MAX_RESULTS 64

typedef struct {
    char ip[16];      /* dotted-quad */
    bool http_open;   /* quick TCP:80 connect succeeded -- hint only */

    /* Best-effort reverse-DNS (PTR) hostname, resolved against the STA's
     * DNS server (normally the router). Empty string if the router didn't
     * answer, doesn't know a name for that host, or has no PTR entry at
     * all -- not every device sends a DHCP hostname to begin with. This is
     * a single plain unicast DNS query, not mDNS (see the scope note at
     * the top of this file for why mDNS itself was left out). */
    char name[32];

    /* Filled in by fingerprint.c for http_open hosts; family stays
     * PLUG_FAMILY_UNKNOWN (and the URLs empty) if nothing matched. */
    plug_family_t family;
    plug_kind_t kind;
    char model[32];
    bool needs_auth;
    char on_url[URL_LEN];
    char off_url[URL_LEN];
    bool use_post;
} net_scan_result_t;

/* NOTE: the full NET_SCAN_MAX_RESULTS-slot array lives only in net_scan.c's
 * internal static state (cheap there -- it's .bss, not a stack/heap alloc).
 * It is intentionally NOT exposed as one struct: an earlier version had a
 * net_scan_status_t with the whole 64-entry array embedded, which callers
 * had to allocate in full on every single status poll (~20KB, since each
 * result grew once fingerprint fields were added) -- that blew a task
 * stack once, and after being "fixed" by heap-allocating it instead, still
 * failed intermittently with ESP_ERR_NO_MEM from heap fragmentation (a
 * 20KB single poll-driven allocation, once a second, on an ESP32 with
 * ~150-200KB of usable heap, is just too much pressure). Fetching one
 * result at a time avoids needing a big buffer anywhere, ever. */

typedef struct {
    bool running;
    bool have_subnet;     /* false if STA was not connected when the scan was requested */
    bool fingerprinting;  /* true while the post-ping fingerprint pass is running */
    uint32_t pass;        /* completed sweep passes so far this run, starting at 1 during the first */
    uint32_t total;       /* hosts to probe per pass */
    uint32_t scanned;     /* hosts probed so far in the current pass (resets every pass) */
    size_t result_count;  /* accumulated across all passes -- a host found once stays found */
} net_scan_summary_t;

/* Must be called once at boot (creates the internal lock; cheap, no task is
 * spawned until net_scan_start()). */
esp_err_t net_scan_init(void);

/* Kicks off a *continuous* ping sweep of the STA subnet in a dedicated task:
 * it repeats the full sweep pass after pass, forever, only stopping when
 * net_scan_stop() is called (or an internal safety cap is hit -- see
 * SCAN_MAX_RUNTIME_MS in net_scan.c, in case a caller forgets to stop it,
 * e.g. the browser tab closes mid-scan). This is intentional: a single
 * pass can miss a device for all sorts of transient reasons (it was still
 * booting, one ping packet got dropped over Wi-Fi, ARP hadn't resolved
 * yet) and previously required the user to notice and click "start" again.
 * Looping fixes that without any extra logic on the caller's side -- a
 * device that didn't answer this pass very likely will in the next one a
 * couple seconds later.
 *
 * A host, once found, is never re-probed or duplicated in later passes
 * (matched by IP) -- results only ever accumulate across passes until
 * net_scan_stop() is called or NET_SCAN_MAX_RESULTS is reached. Poll
 * progress via net_scan_get_summary() / net_scan_get_result() same as
 * before; net_scan_summary_t.pass tells you which sweep pass is current.
 *
 * Returns:
 *   ESP_OK                  scan started
 *   ESP_ERR_INVALID_STATE   a scan is already running
 *   ESP_ERR_NOT_FOUND       STA is not connected, nothing to scan
 */
esp_err_t net_scan_start(void);

/* Requests the running continuous scan to stop. Takes effect within one
 * ping batch (a fraction of a second), not instantly and not necessarily
 * before this call returns -- poll net_scan_get_summary().running to know
 * when it's actually finished. Safe to call when nothing is running
 * (returns ESP_ERR_INVALID_STATE, not an error worth surfacing to the
 * user -- e.g. the "stop"/close button doesn't need to know whether a scan
 * was actually in flight). */
esp_err_t net_scan_stop(void);

void net_scan_get_summary(net_scan_summary_t *out);

/* Copies results[index] into *out. Returns false (*out left untouched) if
 * index >= the result_count reported by net_scan_get_summary(). */
bool net_scan_get_result(size_t index, net_scan_result_t *out);

#ifdef __cplusplus
}
#endif
