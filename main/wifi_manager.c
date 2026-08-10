#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";
#define NVS_NS "wifi"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile bool s_sta_connected = false;
static bool s_have_sta_creds = false;
static char s_sta_ssid[33] = {0};
static char s_sta_pass[65] = {0};

/* After this many consecutive disconnects with a "this won't fix itself by
 * retrying" reason (wrong password, SSID not found, ...), stop auto-
 * reconnecting and let the UI surface it instead of retrying forever. A
 * transient disconnect (AP reboot, roaming, momentary RF noise) does NOT
 * count against this -- those reasons fall through to the unlimited-retry
 * branch below, same as before. */
#define STA_CONFIG_ERROR_MAX_RETRIES 3
static int s_config_error_count = 0;
static volatile bool s_sta_gave_up = false;

#define WIFI_SCAN_TASK_STACK 4096

static SemaphoreHandle_t s_scan_lock;
static bool s_scan_running = false;
static wifi_scan_result_t s_scan_results[WIFI_SCAN_MAX_RESULTS];
static size_t s_scan_count = 0;

static void wifi_scan_task(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_scan_start(NULL, true /* block until done */);

    wifi_scan_result_t results[WIFI_SCAN_MAX_RESULTS];
    size_t count = 0;

    if (err == ESP_OK) {
        /* static, not stack -- ~90-100 bytes/entry * 20 is fine either way,
         * but this keeps the pattern consistent with net_scan.c. */
        static wifi_ap_record_t raw[WIFI_SCAN_MAX_RESULTS];
        uint16_t num = WIFI_SCAN_MAX_RESULTS;
        if (esp_wifi_scan_get_ap_records(&num, raw) == ESP_OK) {
            for (uint16_t i = 0; i < num && count < WIFI_SCAN_MAX_RESULTS; i++) {
                const char *ssid = (const char *)raw[i].ssid;
                if (!ssid[0]) continue; /* hidden network -- nothing to show or pick */

                bool dup = false;
                for (size_t j = 0; j < count; j++) {
                    if (strcmp(results[j].ssid, ssid) == 0) { dup = true; break; }
                }
                /* esp_wifi_scan_get_ap_records() returns RSSI-descending, so
                 * the first occurrence of a given SSID is already its
                 * strongest BSSID -- nothing more to do for duplicates. */
                if (dup) continue;

                snprintf(results[count].ssid, sizeof(results[count].ssid), "%s", ssid);
                results[count].secure = raw[i].authmode != WIFI_AUTH_OPEN;
                count++;
            }
        }
    } else {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
    }

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    memcpy(s_scan_results, results, sizeof(results));
    s_scan_count = count;
    s_scan_running = false;
    xSemaphoreGive(s_scan_lock);

    ESP_LOGI(TAG, "wifi scan finished: %u network(s) found", (unsigned)count);
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_scan_start(void)
{
    if (!s_scan_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (s_scan_running) {
        xSemaphoreGive(s_scan_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_running = true;
    xSemaphoreGive(s_scan_lock);

    BaseType_t ok = xTaskCreate(wifi_scan_task, "wifi_scan", WIFI_SCAN_TASK_STACK, NULL, 3, NULL);
    if (ok != pdPASS) {
        xSemaphoreTake(s_scan_lock, portMAX_DELAY);
        s_scan_running = false;
        xSemaphoreGive(s_scan_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_manager_get_scan_status(bool *running, size_t *count)
{
    if (!s_scan_lock) {
        if (running) *running = false;
        if (count) *count = 0;
        return;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (running) *running = s_scan_running;
    if (count) *count = s_scan_count;
    xSemaphoreGive(s_scan_lock);
}

bool wifi_manager_get_scan_result(size_t index, wifi_scan_result_t *out)
{
    if (!out || !s_scan_lock) return false;
    bool ok = false;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (index < s_scan_count) {
        *out = s_scan_results[index];
        ok = true;
    }
    xSemaphoreGive(s_scan_lock);
    return ok;
}

static bool is_config_error_reason(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_AUTH_FAIL:            /* the classic "wrong PSK" reason */
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_NO_AP_FOUND:          /* SSID typo / AP not in range */
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return true;
        default:
            return false;
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_sta_creds) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (!s_have_sta_creds || s_sta_gave_up) {
            return;
        }
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
        if (evt && is_config_error_reason(evt->reason)) {
            s_config_error_count++;
            ESP_LOGW(TAG, "STA disconnected (reason %u), спроба %d/%d",
                     (unsigned)evt->reason, s_config_error_count, STA_CONFIG_ERROR_MAX_RETRIES);
            if (s_config_error_count >= STA_CONFIG_ERROR_MAX_RETRIES) {
                s_sta_gave_up = true;
                ESP_LOGE(TAG, "STA: вичерпано спроби підключення -- перевірте SSID/пароль");
                return;
            }
        } else {
            ESP_LOGW(TAG, "STA disconnected, retrying");
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_sta_connected = true;
        s_config_error_count = 0;
        s_sta_gave_up = false;
    }
}

static bool load_creds(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t slen = ssid_size, plen = pass_size;
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &slen);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &plen);
    nvs_close(h);

    return (e1 == ESP_OK && ssid[0] != '\0' && e2 == ESP_OK);
}

esp_err_t wifi_manager_init(void)
{
    s_scan_lock = xSemaphoreCreateMutex();
    if (!s_scan_lock) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t el_err = esp_event_loop_create_default();
    if (el_err != ESP_OK && el_err != ESP_ERR_INVALID_STATE) {
        return el_err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    s_have_sta_creds = load_creds(s_sta_ssid, sizeof(s_sta_ssid), s_sta_pass, sizeof(s_sta_pass));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s%02X%02X%02X",
             CONFIG_AP_FALLBACK_SSID_PREFIX, mac[3], mac[4], mac[5]);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    if (strlen(CONFIG_AP_FALLBACK_PASSWORD) >= 8) {
        strncpy((char *)ap_cfg.ap.password, CONFIG_AP_FALLBACK_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    if (s_have_sta_creds) {
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP SSID: %s (password %s)", ap_cfg.ap.ssid,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "<open>" : CONFIG_AP_FALLBACK_PASSWORD);
    if (s_have_sta_creds) {
        ESP_LOGI(TAG, "Connecting to STA SSID: %s", s_sta_ssid);
    } else {
        ESP_LOGW(TAG, "No STA credentials stored yet; configure via web UI on the AP");
    }

    return ESP_OK;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", password ? password : "");
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    memset(s_sta_pass, 0, sizeof(s_sta_pass));
    strncpy(s_sta_pass, password ? password : "", sizeof(s_sta_pass) - 1);
    s_have_sta_creds = true;

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password ? password : "", sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    /* New credentials deserve a fresh set of retry attempts, even if the
     * previous ones had already been given up on. */
    s_config_error_count = 0;
    s_sta_gave_up = false;
    s_sta_connected = false;
    esp_wifi_disconnect();
    esp_wifi_connect();

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_sta_connected;
}

void wifi_manager_get_status(char *sta_ip_out, size_t sta_ip_size,
                              char *ap_ip_out, size_t ap_ip_size,
                              char *ssid_out, size_t ssid_size)
{
    esp_netif_ip_info_t ip;

    if (sta_ip_out && sta_ip_size) {
        if (s_sta_connected && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(sta_ip_out, sta_ip_size, IPSTR, IP2STR(&ip.ip));
        } else {
            snprintf(sta_ip_out, sta_ip_size, "0.0.0.0");
        }
    }
    if (ap_ip_out && ap_ip_size) {
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            snprintf(ap_ip_out, ap_ip_size, IPSTR, IP2STR(&ip.ip));
        } else {
            snprintf(ap_ip_out, ap_ip_size, "0.0.0.0");
        }
    }
    if (ssid_out && ssid_size) {
        snprintf(ssid_out, ssid_size, "%s", s_sta_ssid);
    }
}

void wifi_manager_get_sta_password(char *pass_out, size_t pass_size)
{
    if (!pass_out || !pass_size) return;
    snprintf(pass_out, pass_size, "%s", s_sta_pass);
}

bool wifi_manager_sta_config_error(void)
{
    return s_sta_gave_up;
}

bool wifi_manager_get_sta_ip_info(esp_netif_ip_info_t *out)
{
    if (!s_sta_connected || !out) return false;
    return esp_netif_get_ip_info(s_sta_netif, out) == ESP_OK;
}

bool wifi_manager_get_sta_dns(esp_ip4_addr_t *out)
{
    if (!s_sta_connected || !out) return false;
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return false;
    if (dns.ip.type != ESP_IPADDR_TYPE_V4) return false;
    *out = dns.ip.u_addr.ip4;
    return out->addr != 0;
}
