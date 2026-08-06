#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";
#define NVS_NS "wifi"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile bool s_sta_connected = false;
static bool s_have_sta_creds = false;
static char s_sta_ssid[33] = {0};

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_sta_creds) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_have_sta_creds) {
            ESP_LOGW(TAG, "STA disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_sta_connected = true;
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

    char sta_pass[65] = {0};
    s_have_sta_creds = load_creds(s_sta_ssid, sizeof(s_sta_ssid), sta_pass, sizeof(sta_pass));

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
        strncpy((char *)sta_cfg.sta.password, sta_pass, sizeof(sta_cfg.sta.password) - 1);
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
    s_have_sta_creds = true;

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password ? password : "", sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

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
