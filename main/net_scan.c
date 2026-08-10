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
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
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

/* --- Active ARP probing: the primary discovery mechanism -----------------
 *
 * A host's NIC must answer ARP for basic L2 connectivity to work at all --
 * far more hosts answer "who has this IP" than answer ICMP echo or accept
 * a TCP SYN (default Windows Firewall, plenty of IoT firmware, both drop
 * ICMP/TCP outright while still answering ARP, since they have no choice
 * about the latter). Earlier versions of this scanner pinged the whole
 * subnet and only *incidentally* benefited from ARP (lwIP has to resolve
 * a target's MAC before it can even attempt a ping/TCP connect, so ARP
 * happened as an unavoidable side effect) -- reading that already-happening
 * resolution was cheap and added as a fallback. This version flips it:
 * ARP resolution is triggered *deliberately and first*, for every address
 * in the subnet, and ping is now just a secondary confirmation step run
 * only against hosts ARP already proved are alive (see run_one_pass()).
 *
 * Deliberately NOT done via lwIP's own etharp_request() (which needs a raw
 * `struct netif *`, meaning either exposing wifi_manager's internal STA
 * netif or reaching for esp_netif_get_netif_impl() -- a function whose own
 * header explicitly says it's "supposed to be called only from internals
 * of TCP/IP stack"). Instead: a single UDP socket, reused for the whole
 * scan session, does a plain sendto() to each candidate address on
 * ARP_PROBE_UDP_PORT. The payload and any reply (there usually isn't one --
 * nothing is listening on that port) are irrelevant and never read; the
 * only thing this is for is making lwIP resolve the destination's MAC
 * before it can even attempt to route the packet, which is the exact same
 * mechanism ping/TCP already relied on, just triggered directly through
 * fully public BSD socket calls instead of lwIP-internal ones. sendto()
 * doesn't block waiting for that resolution (lwIP queues the packet and
 * resolves in the background via the tcpip thread), so a whole batch of
 * addresses can be probed back-to-back near-instantly. */
#define ARP_PROBE_BATCH_SIZE 8      /* stays under ARP_TABLE_SIZE (10, see arp_dump_snapshot()) so
                                      * one batch's own replies don't evict each other before the
                                      * settle delay below is up and the table gets read */
#define ARP_PROBE_UDP_PORT   40404  /* arbitrary, high, extremely unlikely to have anything actually
                                      * listening -- doesn't matter either way, see above */
#define ARP_SETTLE_MS        200    /* time to let ARP replies land in the table after a probe batch
                                      * -- generous for a LAN, where real replies land in single-digit
                                      * milliseconds */

/* Secondary confirmation ping, run only against hosts ARP already proved
 * are alive (see run_one_pass()) -- one at a time, not batched/parallel,
 * since by this point the candidate list is typically a handful of real
 * devices rather than a whole /24, so there's no need for the CONCURRENCY-
 * style parallel session juggling the old ping-primary design needed (and
 * which was the original source of "create ping task failed" under load --
 * see README). 250ms was overly generous for a LAN (real replies land in
 * single-digit-to-low-tens of ms); 120ms is still generous while keeping a
 * pass fast, at the cost of maybe missing a genuinely slow/loaded device's
 * ping reply once in a while -- doesn't matter much since this is now only
 * an informational "did it also answer ping" flag, not what gates
 * inclusion in the results at all (ARP presence alone does that). */
#define PING_TIMEOUT_MS        120
#define HTTP_PROBE_TIMEOUT_MS  200
#define HTTP_PROBE_PORT        80
/* Continuous scanning (see net_scan_start()'s doc comment): brief breather
 * between passes so a small/fast subnet doesn't just hammer the network in
 * a tight loop, and a hard safety cap so an abandoned scan (browser tab
 * closed without hitting "stop") doesn't run forever draining the radio. */
#define SCAN_PASS_IDLE_MS      2500
/* Extra breather after each ARP-probe batch *within* a pass -- keeps the
 * pass from being one unbroken tight loop; cheap insurance, same spirit as
 * the old ping-batch breather even though the resource-churn risk that
 * originally motivated it (per-host ping task/socket creation) mostly went
 * away with ping no longer being the primary sweep mechanism. */
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
static SemaphoreHandle_t s_ping_done_sem; /* one ping in flight at a time, see do_single_ping() */
static SemaphoreHandle_t s_arp_done_sem;  /* see arp_dump_snapshot() */
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
    xSemaphoreGive(s_ping_done_sem);
}

/* Actively triggers ARP resolution for up to ARP_PROBE_BATCH_SIZE
 * addresses via plain UDP sendto() calls on the caller-owned socket `sock`
 * -- see the ARP_PROBE_* constants' block comment above for why this
 * approach (rather than lwIP's etharp_request()) was chosen. Fire-and-
 * forget: return value of each sendto() is intentionally ignored, and
 * there is no reply to wait for -- the caller is expected to wait
 * ARP_SETTLE_MS and then check arp_dump_snapshot() to see who answered. */
static void arp_probe_batch(int sock, const uint32_t *ip_h, size_t count)
{
    if (sock < 0) return;
    uint8_t payload = 0;
    for (size_t i = 0; i < count; i++) {
        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(ARP_PROBE_UDP_PORT);
        dst.sin_addr.s_addr = htonl(ip_h[i]);
        sendto(sock, &payload, sizeof(payload), 0, (struct sockaddr *)&dst, sizeof(dst));
    }
}

/* Secondary confirmation ping against a single host already known (via ARP)
 * to be alive -- see the PING_TIMEOUT_MS block comment for why this is now
 * one-at-a-time rather than the old CONCURRENCY-parallel design. Returns
 * whether it replied; a false here is informational only (net_scan_result_t
 * .ping_ok), it does not remove the host from the results -- ARP already
 * proved it's there. */
static bool do_single_ping(uint32_t ip_h)
{
    static ping_ctx_t ctx;
    static esp_ping_handle_t hdl;

    char ip_str[16];
    ip_h_to_str(ip_h, ip_str, sizeof(ip_str));
    ip4_addr_t ip4;
    if (!ip4addr_aton(ip_str, &ip4)) return false;

    ctx.ip_h = ip_h;
    ctx.got_reply = false;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 1;
    cfg.timeout_ms = PING_TIMEOUT_MS;
    cfg.interval_ms = PING_TIMEOUT_MS;
    cfg.data_size = 0;
    cfg.task_stack_size = 3072;
    cfg.task_prio = 2;
    ip_addr_copy_from_ip4(cfg.target_addr, ip4);

    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = NULL,
        .on_ping_end = on_ping_end,
    };

    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) return false;
    if (esp_ping_start(hdl) != ESP_OK) {
        esp_ping_delete_session(hdl);
        return false;
    }
    xSemaphoreTake(s_ping_done_sem, pdMS_TO_TICKS(PING_TIMEOUT_MS + 200));
    return ctx.got_reply;
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

/* --- Reading lwIP's ARP cache ----------------------------------------------
 *
 * The other half of active ARP probing (see the ARP_PROBE_* constants
 * above): arp_probe_batch() triggers resolution, this reads the result.
 * lwIP records every successful ARP resolution in its own table regardless
 * of *why* the resolution happened -- our own deliberate probe, an
 * incidental ping/TCP attempt, or even another device's passing gratuitous
 * ARP/broadcast -- so this doubles as a completely free (no packets of our
 * own) way to notice hosts between probe batches too.
 *
 * ESP-IDF's lwIP port runs with LWIP_TCPIP_CORE_LOCKING *off* by default
 * (see sdkconfig / components/lwip/Kconfig) -- LOCK_TCPIP_CORE()/
 * UNLOCK_TCPIP_CORE() are silent no-ops in that mode, not a real mutex, so
 * calling etharp_get_entry() directly from this task would race against
 * the tcpip thread mutating the same table. The correct, lock-free-required
 * way to touch lwIP internals from any other task is tcpip_callback(): it
 * marshals a function onto the tcpip thread itself and runs it there, so
 * no locking is needed at all. We block (with a timeout, just in case) on
 * a semaphore until that callback has run and copied its result out. */
#define ARP_DUMP_MAX_ENTRIES  32 /* >= lwIP's ARP_TABLE_SIZE (10 by default on this target, not
                                   * exposed via Kconfig -- harmless if the real table is smaller */
#define ARP_CALLBACK_WAIT_MS  500

/* Static, not a local/stack buffer: arp_dump_cb() runs asynchronously on
 * the tcpip thread, some unbounded (if unlikely) amount of time after
 * tcpip_callback() posts it. If arp_dump_snapshot() ever hit its timeout
 * below and returned before the callback actually ran, a stack-local
 * buffer would already be part of some other function's frame by the time
 * the callback finally wrote into it -- silent stack corruption, in a
 * project that has already been bitten hard by exactly that class of bug
 * once (see plug_manager.c/README). A static buffer makes a late callback
 * merely harmless (it just overwrites still-valid memory), not dangerous.
 * Safe because this is only ever called serially from net_scan_task. */
static uint32_t s_arp_ip_h[ARP_DUMP_MAX_ENTRIES];
static size_t s_arp_count;

/* Runs ON the tcpip thread (see the block comment above) -- must not block,
 * must not call anything that isn't safe there. etharp_get_entry(i, ...)
 * returns nonzero for a valid/stable entry at index i, 0 for an empty or
 * still-pending slot; iterating the whole table is the only API lwIP
 * exposes for "dump everything currently known", there's no single-shot
 * "give me them all" call. */
static void arp_dump_cb(void *arg)
{
    (void)arg;
    s_arp_count = 0;
    for (int i = 0; i < ARP_TABLE_SIZE && s_arp_count < ARP_DUMP_MAX_ENTRIES; i++) {
        ip4_addr_t *ipaddr;
        struct netif *netif;
        struct eth_addr *ethaddr;
        if (etharp_get_entry((size_t)i, &ipaddr, &netif, &ethaddr)) {
            s_arp_ip_h[s_arp_count++] = ntohl(ipaddr->addr);
        }
    }
    xSemaphoreGive(s_arp_done_sem);
}

/* Snapshot of whatever's currently in lwIP's ARP cache, host-order IPs.
 * Cheap and safe to call often (e.g. once per ARP-probe batch, see
 * run_one_pass()) -- no network I/O of its own, just one hop onto the
 * tcpip thread and back. Uses the single long-lived s_arp_done_sem rather
 * than creating/deleting a semaphore per call -- this only ever runs
 * serially from within net_scan_task, so one reusable semaphore (created
 * once in net_scan_init(), same lifecycle as s_lock/s_ping_done_sem) is enough
 * and avoids adding heap churn to a loop that already runs many times per
 * pass. Returns the number of entries written to out_ip_h (0 on any
 * failure, including a timeout waiting for the tcpip thread -- not
 * expected in practice, but this must never hang the scan task). */
static size_t arp_dump_snapshot(uint32_t *out_ip_h, size_t max_out)
{
    size_t n = 0;
    if (tcpip_callback(arp_dump_cb, NULL) == ERR_OK &&
        xSemaphoreTake(s_arp_done_sem, pdMS_TO_TICKS(ARP_CALLBACK_WAIT_MS)) == pdTRUE) {
        n = s_arp_count < max_out ? s_arp_count : max_out;
        memcpy(out_ip_h, s_arp_ip_h, n * sizeof(uint32_t));
    }
    return n;
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

static void count_scanned(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.scanned++;
    xSemaphoreGive(s_lock);
}

/* One ARP-probe sweep of [first_h, last_h], skipping hosts already known
 * from an earlier pass -- and, from pass 2 onward, skipping most
 * not-yet-found hosts too (see SCAN_RETRY_STRIDE), only actually probing
 * 1/SCAN_RETRY_STRIDE of them so a long continuous scan doesn't keep
 * re-hammering the whole subnet every ~2.5s.
 *
 * Per batch of up to ARP_PROBE_BATCH_SIZE candidate addresses: trigger ARP
 * resolution (arp_probe_batch()), wait for replies to settle, then read the
 * ARP table (arp_dump_snapshot()) -- any address in there that isn't
 * already known is a newly-discovered host. For each: a single confirmation
 * ping (informational -- .ping_ok, does not gate inclusion) and the usual
 * TCP:80 probe, then appended to results[] (family/kind/name still blank --
 * that's identify_new_results()'s job right after this returns).
 *
 * Returns early without finishing the sweep if a stop is requested
 * mid-pass, same as a normal pass end otherwise. */
static void run_one_pass(int arp_sock, uint32_t ip_h, uint32_t first_h, uint32_t last_h, uint32_t pass_no)
{
    uint32_t batch_ips[ARP_PROBE_BATCH_SIZE];

    for (uint32_t base = first_h; base <= last_h; base += ARP_PROBE_BATCH_SIZE) {
        if (stop_requested_locked()) return;

        size_t batch_n = 0;
        for (uint32_t h = base; h < base + ARP_PROBE_BATCH_SIZE && h <= last_h; h++) {
            if (h == ip_h) { count_scanned(); continue; } /* that's us */

            char ip_str[16];
            ip_h_to_str(h, ip_str, sizeof(ip_str));
            ip4_addr_t ip4;
            if (!ip4addr_aton(ip_str, &ip4)) { count_scanned(); continue; }

            /* Already found in an earlier pass -- don't re-probe it. Still
             * counts as "scanned" for this pass's progress display. */
            if (ip_already_known(ip_str)) { count_scanned(); continue; }

            /* Not yet found, and not the first pass -- only actually probe
             * this host on 1 out of every SCAN_RETRY_STRIDE passes (which
             * one depends on its address, so different hosts get retried on
             * different passes rather than all bunching up together). */
            if (pass_no > 1 && ((h - first_h + pass_no) % SCAN_RETRY_STRIDE) != 0) {
                count_scanned();
                continue;
            }

            batch_ips[batch_n++] = h;
        }

        if (batch_n == 0) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.scanned += batch_n;
        xSemaphoreGive(s_lock);

        arp_probe_batch(arp_sock, batch_ips, batch_n);
        vTaskDelay(pdMS_TO_TICKS(ARP_SETTLE_MS));

        uint32_t arp_ips[ARP_DUMP_MAX_ENTRIES];
        size_t arp_n = arp_dump_snapshot(arp_ips, ARP_DUMP_MAX_ENTRIES);

        for (size_t i = 0; i < arp_n; i++) {
            uint32_t h = arp_ips[i];
            if (h == ip_h || h < first_h || h > last_h) continue;

            char ip_str[16];
            ip_h_to_str(h, ip_str, sizeof(ip_str));
            if (ip_already_known(ip_str)) continue;

            bool ping_ok = do_single_ping(h);
            bool http_open = quick_tcp_probe(h, HTTP_PROBE_PORT, HTTP_PROBE_TIMEOUT_MS);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_status.result_count < NET_SCAN_MAX_RESULTS) {
                net_scan_result_t *r = &s_status.results[s_status.result_count++];
                /* This slot may hold a previous scan session's data (family/
                 * model/URLs) at the same index -- clear it so nothing stale
                 * leaks into a report about a different IP. */
                memset(r, 0, sizeof(*r));
                memcpy(r->ip, ip_str, sizeof(r->ip));
                r->http_open = http_open;
                r->ping_ok = ping_ok;
            }
            xSemaphoreGive(s_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_BATCH_BREATHER_MS));
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

    /* One UDP socket for the whole scan session (all passes), reused for
     * every arp_probe_batch() call -- see the ARP_PROBE_* constants' block
     * comment for why a socket-based probe was chosen over lwIP-internal
     * etharp_request(). Bind to the STA's own address first: this device
     * runs APSTA (its own fallback AP is always up alongside the STA
     * link), so without an explicit bind an outgoing probe could just as
     * easily leave via the SoftAP interface instead of the STA's actual
     * LAN -- same precaution reverse_dns_lookup()'s socket already takes,
     * for the same reason. */
    int arp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (arp_sock < 0) {
        ESP_LOGW(TAG, "could not open ARP-probe socket (errno=%d) -- scan will only find hosts "
                 "that answer ARP passively (gratuitous/other-host traffic), not ones actively probed", errno);
    } else {
        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = ip_info.ip.addr;
        bind(arp_sock, (struct sockaddr *)&local, sizeof(local));
    }

    ESP_LOGI(TAG, "continuous scan starting: %u hosts around " IPSTR "/%u per pass",
             (unsigned)total, IP2STR(&ip_info.ip), (unsigned)__builtin_popcount(mask_h));
    /* Internal SRAM specifically -- task stacks can't be satisfied from
     * PSRAM, so the combined "free heap" figure would be misleading here.
     * Several other tasks permanently reserve a large stack each
     * regardless of whether they're doing anything at the moment -- see
     * plug_manager.c/README for the fuller history of why this is worth
     * keeping an eye on. */
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
        run_one_pass(arp_sock, ip_h, first_h, last_h, pass_no);
        if (stop_requested_locked()) break;

        identify_new_results(pass_start_count);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t found_now = s_status.result_count;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "pass %u finished: %u total host(s) found so far", (unsigned)pass_no, (unsigned)found_now);

        if (stop_requested_locked()) break;
        vTaskDelay(pdMS_TO_TICKS(SCAN_PASS_IDLE_MS));
    }

    if (arp_sock >= 0) close(arp_sock);

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
    s_ping_done_sem = xSemaphoreCreateBinary();
    s_arp_done_sem = xSemaphoreCreateBinary();
    if (!s_lock || !s_ping_done_sem || !s_arp_done_sem) return ESP_ERR_NO_MEM;
    memset(&s_status, 0, sizeof(s_status));

    /* A ping_sock session-creation failure here is not a bug -- it's fully
     * handled already (do_single_ping() just returns false, and that host
     * still gets added to results[] with ping_ok=false since ARP already
     * proved it exists -- ping is only a secondary confirmation now, it no
     * longer gates discovery). The component's own ESP_LOGE for a session-
     * creation failure is just alarming console noise for something this
     * design already tolerates gracefully. */
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

bool net_scan_is_responsive(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    xSemaphoreGive(s_lock);
    return true;
}
