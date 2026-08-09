#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "ping/ping_sock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "wifi_manager.h"
#include "fingerprint.h"
#include "net_scan.h"

static const char *TAG = "net_scan";

/* How many ping sessions run at once. Each spins up its own small FreeRTOS
 * task internally (esp_ping) AND holds a raw socket for its lifetime, so
 * this bounds peak RAM/task/socket usage rather than sweeping all ~254
 * hosts of a /24 concurrently. Also competes for the same LWIP socket pool
 * (CONFIG_LWIP_MAX_SOCKETS, see sdkconfig.defaults) as the httpd server, AP
 * DHCP, and fingerprint.c's HTTP client.
 *
 * History: was 4 (already reduced once from 8 for the same "create ping
 * task/socket failed" reason), then 2, then 1 (fully serial) while chasing
 * that error down -- see net_scan_init()'s comment for why a session-
 * creation failure was never actually unsafe, just noisy. Real hardware
 * logs showed 50KB+ free internal SRAM at scan start and the handful of
 * failures that did happen at CONCURRENCY=2 all coincided with an unrelated
 * one-time SNTP sync -- i.e. rare transient contention, not a hard ceiling.
 * Back up to 2 as the speed/safety tradeoff: still far short of the
 * original 4 that failed badly, but noticeably faster than dead serial. */
#define CONCURRENCY            2
/* How long to wait for a reply before giving up on a host. 250ms was overly
 * generous for a LAN (real replies land in single-digit-to-low-tens of ms);
 * 120ms cuts a full pass's dead time roughly in half for the (usual) case
 * of a mostly-empty subnet, at the cost of maybe missing a genuinely
 * slow/loaded device once in a while -- acceptable since continuous
 * scanning (see net_scan_start()) retries it again a few passes later
 * anyway (SCAN_RETRY_STRIDE), it's not a one-shot final answer. */
#define PING_TIMEOUT_MS        120
#define HTTP_PROBE_TIMEOUT_MS  200
#define HTTP_PROBE_PORT        80
/* Continuous scanning (see net_scan_start()'s doc comment): brief breather
 * between passes so a small/fast subnet doesn't just hammer the network in
 * a tight loop, and a hard safety cap so an abandoned scan (browser tab
 * closed without hitting "stop") doesn't run forever draining the radio. */
#define SCAN_PASS_IDLE_MS      2500
/* Extra breather between each CONCURRENCY-sized ping batch *within* a
 * pass -- gives FreeRTOS's idle task a moment to actually reclaim each
 * batch's just-deleted ping session tasks before the next batch asks for
 * new ones, instead of pure back-to-back create/delete churn. */
#define SCAN_BATCH_BREATHER_MS 15
/* From the 2nd pass onward, a host that has never once responded is only
 * actually pinged in 1 out of this many passes (still counted as "scanned"
 * on the passes it's skipped) -- retries keep happening (the whole point of
 * continuous scanning), just spread out instead of re-hammering the same
 * ~250 mostly-empty addresses every single pass, which was the other big
 * contributor to the session-creation churn above. The very first pass
 * always covers the full subnet, same as the old one-shot behavior. */
#define SCAN_RETRY_STRIDE       3
#define SCAN_MAX_RUNTIME_MS    (10 * 60 * 1000)
/* esp_http_client (DNS, TCP, header parsing) needs considerably more than
 * 4KB -- plug_manager.c's worker task hit this exact stack-overflow before
 * being bumped to 8192; the fingerprint pass below uses esp_http_client
 * too, so this task needs the same headroom. 8192 turned out to still be
 * too little on ESP32-S3 (see the longer comment in plug_manager.c) --
 * bumped to match. */
#define SCAN_TASK_STACK        12288

/* Private, file-scope-only mirror of the old public net_scan_status_t --
 * cheap to keep here since it's .bss, not something any caller has to
 * allocate. Callers only ever see it through net_scan_get_summary() /
 * net_scan_get_result(), copied one small piece at a time. */
typedef struct {
    bool running;
    bool have_subnet;
    bool fingerprinting;
    bool stop_requested;
    uint32_t pass;
    uint32_t total;
    uint32_t scanned;
    size_t result_count;
    net_scan_result_t results[NET_SCAN_MAX_RESULTS];
} net_scan_full_status_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_batch_sem;
static net_scan_full_status_t s_status;

typedef struct {
    uint32_t ip_h;         /* host order */
    volatile bool got_reply;
} ping_ctx_t;

static void ip_h_to_str(uint32_t ip_h, char *out, size_t out_size)
{
    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned)((ip_h >> 24) & 0xFF), (unsigned)((ip_h >> 16) & 0xFF),
             (unsigned)((ip_h >> 8) & 0xFF), (unsigned)(ip_h & 0xFF));
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    ctx->got_reply = true;
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    /* Deleting the session from within its own end-callback is the pattern
     * used by ESP-IDF's own ping example/component. */
    esp_ping_delete_session(hdl);
    xSemaphoreGive(s_batch_sem);
}

/* Best-effort non-blocking TCP connect used only as a hint ("this host has
 * something answering on :80, might be worth a look"), not as a guarantee
 * of anything. */
static bool quick_tcp_probe(uint32_t ip_h, uint16_t port, int timeout_ms)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip_h);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    bool ok = false;
    int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                ok = true;
            }
        }
    }
    close(sock);
    return ok;
}

/* --- Reverse-DNS (PTR) hostname lookup ------------------------------------
 *
 * Two-stage lookup, same PTR wire format both times, just a different
 * destination:
 *
 * 1. Plain unicast DNS query to whatever DNS server the STA's DHCP lease
 *    handed us (normally the router itself). Works when the router is
 *    dnsmasq-based (OpenWrt and similar) and proxies local PTR queries for
 *    its own DHCP clients.
 * 2. If that comes back empty -- which turned out to be the common case:
 *    plenty of consumer/mesh routers show device names in their OWN admin
 *    UI (from the DHCP lease table, or their own app/cloud service) but
 *    don't expose that over queryable DNS at all -- fall back to asking
 *    the device itself directly over mDNS (RFC 6762): a multicast query to
 *    224.0.0.251:5353, with the "QU" unicast-response bit set so the
 *    reply comes straight back to us and we don't need to join the
 *    multicast group to receive it. Phones (Android/iOS), laptops (Bonjour/
 *    avahi) and plenty of IoT firmware (Shelly Gen2+, ESPHome) all run an
 *    mDNS responder and will answer this even when the router won't. This
 *    is a single hand-rolled query/response pair, not the esp-idf `mdns`
 *    managed component (see the scope note at the top of this file for why
 *    that was left out) -- reuses the exact same packet building/parsing
 *    as the unicast path below. */

#define DNS_QUERY_TIMEOUT_MS 300
#define MDNS_MULTICAST_IP_H  0xE00000FBu  /* 224.0.0.251, host order */
#define MDNS_PORT            5353

static size_t dns_write_qname_ptr(uint32_t ip_h, uint8_t *out)
{
    char qname[32];
    snprintf(qname, sizeof(qname), "%u.%u.%u.%u.in-addr.arpa",
             (unsigned)(ip_h & 0xFF), (unsigned)((ip_h >> 8) & 0xFF),
             (unsigned)((ip_h >> 16) & 0xFF), (unsigned)((ip_h >> 24) & 0xFF));

    size_t pos = 0;
    const char *label = qname;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (len == 0 || len > 63) break;
        out[pos++] = (uint8_t)len;
        memcpy(&out[pos], label, len);
        pos += len;
        label += len;
        if (*label == '.') label++;
    }
    out[pos++] = 0; /* root label */
    return pos;
}

/* Reads the (possibly compressed) DNS name at msg[offset]. If out/out_size
 * are given, writes the dot-joined decoded name there (best-effort --
 * silently drops labels that don't fit). Returns the offset just past the
 * name's FIRST occurrence in the stream (i.e. where the next field starts),
 * or 0 on malformed/truncated input. Follows compression pointers with a
 * hop limit, so malformed pointer loops from an untrusted LAN peer can't
 * hang this. */
static size_t dns_read_name(const uint8_t *msg, size_t msg_len, size_t offset,
                             char *out, size_t out_size)
{
    size_t pos = offset;
    size_t out_len = 0;
    int jumps = 0;
    size_t ret_pos = 0;

    if (out && out_size) out[0] = '\0';

    while (pos < msg_len) {
        uint8_t len = msg[pos];
        if (len == 0) {
            pos++;
            if (ret_pos == 0) ret_pos = pos;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= msg_len) return 0;
            if (ret_pos == 0) ret_pos = pos + 2;
            if (++jumps > 5) return 0;
            pos = (size_t)((len & 0x3F) << 8) | msg[pos + 1];
            continue;
        }
        if ((len & 0xC0) != 0) return 0; /* reserved label type */
        pos++;
        if (pos + len > msg_len) return 0;
        if (out && out_len + len + 1 < out_size) {
            if (out_len) out[out_len++] = '.';
            memcpy(&out[out_len], &msg[pos], len);
            out_len += len;
            out[out_len] = '\0';
        }
        pos += len;
    }
    return ret_pos;
}

/* Shared by both the unicast (to the router) and mDNS (multicast, QU bit)
 * variants below -- identical PTR query/response wire format, only the
 * destination and QCLASS differ. */
static bool dns_ptr_query(const char *ip_str, uint32_t dest_ip_h, uint16_t dest_port,
                           uint16_t qclass, char *out, size_t out_size)
{
    out[0] = '\0';

    ip4_addr_t target;
    if (!ip4addr_aton(ip_str, &target)) return false;
    uint32_t ip_h = ntohl(target.addr);

    uint8_t pkt[64];
    uint16_t qid = (uint16_t)(esp_random() & 0xFFFF);
    pkt[0] = (uint8_t)(qid >> 8); pkt[1] = (uint8_t)(qid & 0xFF);
    pkt[2] = 0x01; pkt[3] = 0x00;  /* standard query, recursion desired */
    pkt[4] = 0x00; pkt[5] = 0x01;  /* QDCOUNT = 1 */
    pkt[6] = 0x00; pkt[7] = 0x00;
    pkt[8] = 0x00; pkt[9] = 0x00;
    pkt[10] = 0x00; pkt[11] = 0x00;
    size_t off = 12;
    off += dns_write_qname_ptr(ip_h, &pkt[off]);
    pkt[off++] = 0x00; pkt[off++] = 0x0C; /* QTYPE = PTR */
    pkt[off++] = (uint8_t)(qclass >> 8); pkt[off++] = (uint8_t)(qclass & 0xFF);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    /* Bind to the STA's own address before sending. This device runs
     * APSTA (its own fallback AP is always up alongside the STA link), so
     * without an explicit bind the outgoing packet -- especially the
     * multicast one -- could just as easily leave via the SoftAP
     * interface instead of the STA's actual LAN, which would silently
     * fail every single query for no obvious reason. */
    esp_netif_ip_info_t sta_ip;
    if (wifi_manager_get_sta_ip_info(&sta_ip)) {
        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = sta_ip.ip.addr;
        bind(sock, (struct sockaddr *)&local, sizeof(local));
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = DNS_QUERY_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dest_port);
    dst.sin_addr.s_addr = htonl(dest_ip_h);

    bool ok = false;
    if (sendto(sock, pkt, off, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)off) {
        uint8_t resp[256];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, resp, sizeof(resp), 0, (struct sockaddr *)&from, &from_len);
        /* Only require the transaction ID to match for the unicast/router
         * path -- mDNS responders are explicitly allowed (RFC 6762 s18.1)
         * to reply with a different ID, since multicast queries aren't
         * meant to rely on it. Either way only one query is ever in
         * flight per socket, so accepting any well-formed reply here is
         * safe. */
        if (n > 12) {
            uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
            if (ancount > 0) {
                size_t after_q = dns_read_name(resp, (size_t)n, 12, NULL, 0);
                if (after_q && after_q + 4 <= (size_t)n) {
                    size_t rr_pos = after_q + 4; /* past question's QTYPE+QCLASS */
                    size_t after_name = dns_read_name(resp, (size_t)n, rr_pos, NULL, 0);
                    if (after_name && after_name + 10 <= (size_t)n) {
                        uint16_t rtype = (uint16_t)((resp[after_name] << 8) | resp[after_name + 1]);
                        uint16_t rdlen = (uint16_t)((resp[after_name + 8] << 8) | resp[after_name + 9]);
                        size_t rdata_off = after_name + 10;
                        if (rtype == 12 && rdata_off + rdlen <= (size_t)n) {
                            char name[64];
                            if (dns_read_name(resp, (size_t)n, rdata_off, name, sizeof(name)) && name[0]) {
                                strncpy(out, name, out_size - 1);
                                out[out_size - 1] = '\0';
                                ok = true;
                            }
                        }
                    }
                }
            }
        }
    }
    close(sock);
    return ok;
}

static bool reverse_dns_lookup(const char *ip_str, uint32_t dns_ip_h, char *out, size_t out_size)
{
    if (dns_ip_h != 0 && dns_ptr_query(ip_str, dns_ip_h, 53, 0x0001, out, out_size)) {
        ESP_LOGD(TAG, "%s: resolved via router DNS", ip_str);
        return true;
    }
    if (dns_ptr_query(ip_str, MDNS_MULTICAST_IP_H, MDNS_PORT, 0x8001, out, out_size)) {
        ESP_LOGD(TAG, "%s: resolved via mDNS", ip_str);
        return true;
    }
    return false;
}

static bool stop_requested_locked(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stop = s_status.stop_requested;
    xSemaphoreGive(s_lock);
    return stop;
}

/* True if ip_str is already in results[] -- a host found in an earlier pass
 * is never re-probed or duplicated, only ever left in place (see
 * net_scan_start()'s doc comment). */
static bool ip_already_known(const char *ip_str)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool known = false;
    for (size_t i = 0; i < s_status.result_count; i++) {
        if (strcmp(s_status.results[i].ip, ip_str) == 0) { known = true; break; }
    }
    xSemaphoreGive(s_lock);
    return known;
}

/* One ping sweep of [first_h, last_h], skipping hosts already known from an
 * earlier pass -- and, from pass 2 onward, skipping most not-yet-found
 * hosts too (see SCAN_RETRY_STRIDE), only actually pinging 1/SCAN_RETRY_STRIDE
 * of them so a long continuous scan doesn't keep re-hammering the whole
 * subnet every ~2.5s. Appends newly-responding hosts to results[] (with
 * http_open filled in; family/kind/name still blank -- that's the caller's
 * job right after this returns). Returns early without finishing the sweep
 * if a stop is requested mid-pass, same as a normal pass end otherwise. */
static void run_one_pass(uint32_t ip_h, uint32_t first_h, uint32_t last_h, uint32_t pass_no)
{
    static ping_ctx_t ctxs[CONCURRENCY];
    static esp_ping_handle_t hdls[CONCURRENCY];

    for (uint32_t base = first_h; base <= last_h; base += CONCURRENCY) {
        if (stop_requested_locked()) return;

        int started = 0;

        for (int i = 0; i < CONCURRENCY; i++) {
            uint32_t h = base + (uint32_t)i;
            if (h > last_h) break;
            if (h == ip_h) {
                /* that's us -- don't ping ourselves, just count it as scanned */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.scanned++;
                xSemaphoreGive(s_lock);
                continue;
            }

            char ip_str[16];
            ip_h_to_str(h, ip_str, sizeof(ip_str));
            ip4_addr_t ip4;
            if (!ip4addr_aton(ip_str, &ip4)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.scanned++;
                xSemaphoreGive(s_lock);
                continue;
            }

            /* Already found in an earlier pass -- don't waste a ping
             * session/socket re-checking it. Still counts as "scanned" for
             * this pass's progress display. */
            if (ip_already_known(ip_str)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.scanned++;
                xSemaphoreGive(s_lock);
                continue;
            }

            /* Not yet found, and not the first pass -- only actually ping
             * this host on 1 out of every SCAN_RETRY_STRIDE passes (which
             * one depends on its address, so different hosts get retried on
             * different passes rather than all bunching up together). */
            if (pass_no > 1 && ((h - first_h + pass_no) % SCAN_RETRY_STRIDE) != 0) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.scanned++;
                xSemaphoreGive(s_lock);
                continue;
            }

            ctxs[started].ip_h = h;
            ctxs[started].got_reply = false;

            esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
            cfg.count = 1;
            cfg.timeout_ms = PING_TIMEOUT_MS;
            cfg.interval_ms = PING_TIMEOUT_MS;
            cfg.data_size = 0;
            cfg.task_stack_size = 3072;
            cfg.task_prio = 2;
            ip_addr_copy_from_ip4(cfg.target_addr, ip4);

            esp_ping_callbacks_t cbs = {
                .cb_args = &ctxs[started],
                .on_ping_success = on_ping_success,
                .on_ping_timeout = NULL,
                .on_ping_end = on_ping_end,
            };

            if (esp_ping_new_session(&cfg, &cbs, &hdls[started]) == ESP_OK) {
                if (esp_ping_start(hdls[started]) == ESP_OK) {
                    started++;
                } else {
                    esp_ping_delete_session(hdls[started]);
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_status.scanned++;
                    xSemaphoreGive(s_lock);
                }
            } else {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.scanned++;
                xSemaphoreGive(s_lock);
            }
        }

        for (int i = 0; i < started; i++) {
            xSemaphoreTake(s_batch_sem, portMAX_DELAY);
        }

        for (int i = 0; i < started; i++) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.scanned++;
            xSemaphoreGive(s_lock);

            if (!ctxs[i].got_reply) continue;

            bool http_open = quick_tcp_probe(ctxs[i].ip_h, HTTP_PROBE_PORT, HTTP_PROBE_TIMEOUT_MS);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_status.result_count < NET_SCAN_MAX_RESULTS) {
                net_scan_result_t *r = &s_status.results[s_status.result_count++];
                /* This slot may hold a previous scan session's data (family/
                 * model/URLs) at the same index -- clear it so nothing stale
                 * leaks into a report about a different IP. */
                memset(r, 0, sizeof(*r));
                ip_h_to_str(ctxs[i].ip_h, r->ip, sizeof(r->ip));
                r->http_open = http_open;
            }
            xSemaphoreGive(s_lock);
        }

        /* See SCAN_BATCH_BREATHER_MS's comment -- only worth the delay if
         * this batch actually created ping sessions to clean up after. */
        if (started > 0) {
            vTaskDelay(pdMS_TO_TICKS(SCAN_BATCH_BREATHER_MS));
        }
    }
}

/* Reverse-DNS + fingerprint pass, run only against results[first..count) --
 * i.e. only the hosts newly found in the pass that just finished. Hosts
 * found in an earlier pass were already fingerprinted/named then and are
 * never touched again, so this stays cheap even after many passes. */
static void identify_new_results(size_t first)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.fingerprinting = true;
    size_t n_new = s_status.result_count - first;
    static char ips[NET_SCAN_MAX_RESULTS][16];
    static bool http_flags[NET_SCAN_MAX_RESULTS];
    for (size_t i = 0; i < n_new; i++) {
        memcpy(ips[i], s_status.results[first + i].ip, sizeof(ips[i]));
        http_flags[i] = s_status.results[first + i].http_open;
    }
    xSemaphoreGive(s_lock);

    if (n_new == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.fingerprinting = false;
        xSemaphoreGive(s_lock);
        return;
    }

    /* Reverse-DNS naming: try every newly-responding host (not just the
     * http_open ones -- a phone or laptop with no web server can still have
     * a DHCP hostname worth showing), against the STA's DNS server. Fetched
     * fresh each pass in case the STA reconnected with a different lease --
     * it's just a struct copy, not network I/O, so there's no cost to that. */
    esp_ip4_addr_t dns_addr;
    bool have_dns = wifi_manager_get_sta_dns(&dns_addr);
    uint32_t dns_ip_h = have_dns ? ntohl(dns_addr.addr) : 0;
    if (have_dns) {
        ESP_LOGI(TAG, "reverse-DNS: querying " IPSTR " for %u new host(s)", IP2STR(&dns_addr), (unsigned)n_new);
    } else {
        ESP_LOGW(TAG, "reverse-DNS: no DNS server known for STA -- skipping naming pass entirely");
    }
    size_t names_found = 0;
    for (size_t i = 0; i < n_new; i++) {
        char name[32];
        if (!reverse_dns_lookup(ips[i], dns_ip_h, name, sizeof(name))) {
            if (have_dns) ESP_LOGD(TAG, "%s -> no PTR name", ips[i]);
            continue;
        }
        names_found++;
        ESP_LOGI(TAG, "%s -> \"%s\"", ips[i], name);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t j = 0; j < s_status.result_count; j++) {
            if (strcmp(s_status.results[j].ip, ips[i]) == 0) {
                memcpy(s_status.results[j].name, name, sizeof(s_status.results[j].name));
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }
    if (have_dns) {
        ESP_LOGI(TAG, "reverse-DNS: resolved %u/%u new host name(s)", (unsigned)names_found, (unsigned)n_new);
    }

    size_t http_candidates = 0;
    for (size_t i = 0; i < n_new; i++) if (http_flags[i]) http_candidates++;
    ESP_LOGI(TAG, "fingerprinting %u new host(s) with :80 open", (unsigned)http_candidates);

    for (size_t i = 0; i < n_new; i++) {
        if (!http_flags[i]) continue;

        plug_fingerprint_t fp;
        fingerprint_probe(ips[i], &fp);
        if (fp.family == PLUG_FAMILY_UNKNOWN) {
            ESP_LOGI(TAG, "%s -> :80 open but not recognized (Shelly/Tasmota/ESPHome checks all failed)", ips[i]);
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        /* Re-find by IP rather than trusting the index -- a concurrent pass
         * never runs (this all happens sequentially in net_scan_task), but
         * matching by value is cheap and one less thing to get wrong if
         * that ever changes. */
        for (size_t j = 0; j < s_status.result_count; j++) {
            if (strcmp(s_status.results[j].ip, ips[i]) == 0) {
                net_scan_result_t *r = &s_status.results[j];
                r->family = fp.family;
                r->kind = fp.kind;
                r->needs_auth = fp.needs_auth;
                r->use_post = fp.use_post;
                memcpy(r->model, fp.model, sizeof(r->model));
                memcpy(r->on_url, fp.on_url, sizeof(r->on_url));
                memcpy(r->off_url, fp.off_url, sizeof(r->off_url));
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.fingerprinting = false;
    xSemaphoreGive(s_lock);
}

static void net_scan_task(void *arg)
{
    (void)arg;
    esp_netif_ip_info_t ip_info;
    if (!wifi_manager_get_sta_ip_info(&ip_info)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.running = false;
        s_status.have_subnet = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "scan requested but STA is not connected");
        vTaskDelete(NULL);
        return;
    }

    uint32_t ip_h = ntohl(ip_info.ip.addr);
    uint32_t mask_h = ntohl(ip_info.netmask.addr);
    uint32_t net_h = ip_h & mask_h;
    uint32_t host_span = ~mask_h;

    if (host_span == 0 || host_span > 254) {
        /* Not a plain /24-ish LAN (huge mask, or point-to-point /32) -- fall
         * back to just the /24 around our own address instead of trying to
         * host-scan something like a /16. */
        net_h = ip_h & 0xFFFFFF00u;
        host_span = 0xFF;
    }
    uint32_t first_h = net_h + 1;              /* skip network address */
    uint32_t last_h = net_h + host_span - 1;    /* skip broadcast address */
    uint32_t total = (last_h >= first_h) ? (last_h - first_h + 1) : 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.have_subnet = true;
    s_status.total = total;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "continuous scan starting: %u hosts around " IPSTR "/%u per pass",
             (unsigned)total, IP2STR(&ip_info.ip), (unsigned)__builtin_popcount(mask_h));
    /* Internal SRAM specifically -- task stacks (incl. the ping sessions
     * about to be created) can't be satisfied from PSRAM, so the combined
     * "free heap" figure would be misleading here. See CONCURRENCY's
     * comment for why this can run low: several other tasks permanently
     * reserve a large stack each regardless of whether they're doing
     * anything at the moment. */
    ESP_LOGI(TAG, "free internal SRAM before scan: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    int64_t start_us = esp_timer_get_time();

    while (!stop_requested_locked()) {
        if (esp_timer_get_time() - start_us > (int64_t)SCAN_MAX_RUNTIME_MS * 1000) {
            ESP_LOGW(TAG, "continuous scan hit the %d-minute safety cap, stopping automatically",
                     (int)(SCAN_MAX_RUNTIME_MS / 60000));
            break;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.pass++;
        s_status.scanned = 0;
        size_t pass_start_count = s_status.result_count;
        uint32_t pass_no = s_status.pass;
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "pass %u starting (%u known so far)", (unsigned)pass_no, (unsigned)pass_start_count);
        run_one_pass(ip_h, first_h, last_h, pass_no);
        if (stop_requested_locked()) break;

        identify_new_results(pass_start_count);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t found_now = s_status.result_count;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "pass %u finished: %u total host(s) found so far", (unsigned)pass_no, (unsigned)found_now);

        if (stop_requested_locked()) break;
        vTaskDelay(pdMS_TO_TICKS(SCAN_PASS_IDLE_MS));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.fingerprinting = false;
    s_status.running = false;
    ESP_LOGI(TAG, "continuous scan stopped after %u pass(es): %u host(s) found",
             (unsigned)s_status.pass, (unsigned)s_status.result_count);
    xSemaphoreGive(s_lock);

    vTaskDelete(NULL);
}

esp_err_t net_scan_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_batch_sem = xSemaphoreCreateCounting(CONCURRENCY, 0);
    if (!s_lock || !s_batch_sem) return ESP_ERR_NO_MEM;
    memset(&s_status, 0, sizeof(s_status));

    /* A ping_sock session-creation failure here is not a bug -- it's fully
     * handled already: that host just gets treated as "no reply this
     * attempt" and, since it's not in results[], SCAN_RETRY_STRIDE makes
     * sure it's tried again on a later pass (see run_one_pass()). The
     * component's own ESP_LOGE for it is just alarming console noise for
     * something the continuous scan already recovers from on its own. */
    esp_log_level_set("ping_sock", ESP_LOG_NONE);

    return ESP_OK;
}

esp_err_t net_scan_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.running) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t probe;
    if (!wifi_manager_get_sta_ip_info(&probe)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    /* Fresh session -- clear everything left over from a previous run (only
     * reachable here at all once that one has actually stopped, since
     * s_status.running gates re-entry above). */
    s_status.running = true;
    s_status.stop_requested = false;
    s_status.fingerprinting = false;
    s_status.pass = 0;
    s_status.scanned = 0;
    s_status.result_count = 0;
    xSemaphoreGive(s_lock);

    BaseType_t ok = xTaskCreate(net_scan_task, "net_scan", SCAN_TASK_STACK, NULL, 3, NULL);
    if (ok != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.running = false;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t net_scan_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_running = s_status.running;
    s_status.stop_requested = true;
    xSemaphoreGive(s_lock);
    return was_running ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void net_scan_get_summary(net_scan_summary_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->running = s_status.running;
    out->have_subnet = s_status.have_subnet;
    out->fingerprinting = s_status.fingerprinting;
    out->pass = s_status.pass;
    out->total = s_status.total;
    out->scanned = s_status.scanned;
    out->result_count = s_status.result_count;
    xSemaphoreGive(s_lock);
}

bool net_scan_get_result(size_t index, net_scan_result_t *out)
{
    if (!out) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index < s_status.result_count) {
        *out = s_status.results[index];
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}
