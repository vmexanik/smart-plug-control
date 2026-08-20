#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "captive_dns.h"

static const char *TAG = "captive_dns";

#define DNS_PORT     53
#define DNS_MAX_LEN  512
/* AP not up yet / socket hiccup -- how long to wait before trying again. */
#define RETRY_DELAY_MS 2000

/* Turns the query already sitting in buf[0..qlen) into an answer in place:
 * same transaction ID and echoed question (both already there, untouched),
 * header flags flipped to "response, no error", ANCOUNT=1, one A record
 * (name = pointer back to the question, per RFC1035 message compression)
 * pointing at ip_be. Deliberately ignores QTYPE/QCLASS -- always answers A
 * -- nothing on this network needs real DNS besides the OS captive-portal
 * probes this exists to catch, and they only care that *something*
 * resolves. */
static int build_reply(uint8_t *buf, int qlen, uint32_t ip_be)
{
    buf[2] = 0x81; /* QR=1 (response), Opcode=0, AA=1, TC=0, RD=1 */
    buf[3] = 0x80; /* RA=1, Z=0, RCODE=0 (no error) */
    buf[6] = 0x00; buf[7] = 0x01; /* ANCOUNT = 1 */
    buf[8] = 0x00; buf[9] = 0x00; /* NSCOUNT = 0 */
    buf[10] = 0x00; buf[11] = 0x00; /* ARCOUNT = 0 */

    int pos = qlen;
    buf[pos++] = 0xC0; buf[pos++] = 0x0C; /* NAME: pointer to the name at offset 12 */
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* TYPE = A */
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* CLASS = IN */
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x3C; /* TTL = 60s */
    buf[pos++] = 0x00; buf[pos++] = 0x04; /* RDLENGTH = 4 */
    memcpy(&buf[pos], &ip_be, 4);
    pos += 4;
    return pos;
}

/* Walks the question section starting at offset 12 to find where it ends
 * (a query has exactly one question). Returns -1 if the packet is too
 * short/malformed to trust. Deliberately refuses name compression in a
 * QUESTION (buf[i] with the top two bits set) -- that's legal in replies,
 * not in real-world queries, and bailing out is simpler and safer than
 * chasing a pointer here. */
static int question_end(const uint8_t *buf, int len)
{
    int i = 12;
    while (i < len && buf[i] != 0x00) {
        int label_len = buf[i];
        if (label_len & 0xC0) return -1;
        i += 1 + label_len;
    }
    if (i >= len) return -1;
    i += 1;      /* the terminating zero-length label */
    i += 4;      /* QTYPE(2) + QCLASS(2) */
    return (i <= len) ? i : -1;
}

static void captive_dns_task(void *arg)
{
    (void)arg;
    static uint8_t buf[DNS_MAX_LEN];

    for (;;) {
        esp_netif_ip_info_t ap_ip;
        if (!wifi_manager_get_ap_ip_info(&ap_ip) || ap_ip.ip.addr == 0) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        /* Bind to the AP's own IP specifically, not INADDR_ANY -- this is
         * what keeps this socket scoped to AP-side traffic only. STA-side
         * DNS (fingerprint.c's HTTP calls resolve IPs directly so they
         * don't even use DNS, but NTP and net_scan.c's reverse-DNS lookups
         * do) goes through lwip's own resolver against whatever DNS server
         * the STA's DHCP lease handed out, a completely separate path that
         * never touches this socket. */
        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = ap_ip.ip.addr;
        local.sin_port = htons(DNS_PORT);
        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            ESP_LOGW(TAG, "bind to AP IP:%d failed, retrying", DNS_PORT);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "captive-portal DNS hijack listening on " IPSTR ":%d", IP2STR(&ap_ip.ip), DNS_PORT);

        bool socket_ok = true;
        while (socket_ok) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (len < 0) {
                ESP_LOGW(TAG, "recvfrom failed, reopening socket");
                socket_ok = false;
                break;
            }
            if (len < 12) continue; /* shorter than a DNS header -- garbage, ignore */

            int qend = question_end(buf, len);
            if (qend < 0 || qend + 10 > (int)sizeof(buf)) continue; /* +10 = the answer RR we're about to append */

            int reply_len = build_reply(buf, qend, ap_ip.ip.addr);
            sendto(sock, buf, reply_len, 0, (struct sockaddr *)&from, fromlen);
        }
        close(sock);
    }
}

esp_err_t captive_dns_init(void)
{
    BaseType_t ok = xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
