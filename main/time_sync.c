#include <string.h>
#include <sys/time.h>
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

/* esp_sntp_config_t only stores the pointer, not a copy -- keep the server
 * string alive for as long as SNTP might be (re)using it. */
static char s_ntp_server[64];
static volatile bool s_synced = false;
static volatile bool s_via_ntp = false;
static bool s_initialized = false;

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    s_via_ntp = true;
    ESP_LOGI(TAG, "Time synchronized via NTP (%s)", s_ntp_server);
}

esp_err_t time_sync_init(const char *ntp_server)
{
    if (s_initialized) {
        esp_netif_sntp_deinit();
        s_initialized = false;
    }

    if (!ntp_server || ntp_server[0] == '\0') {
        ESP_LOGW(TAG, "No NTP server configured, only manual time set is available");
        s_ntp_server[0] = '\0';
        return ESP_OK;
    }

    strncpy(s_ntp_server, ntp_server, sizeof(s_ntp_server) - 1);
    s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    config.sync_cb = on_time_sync;
    config.wait_for_sync = false;
    config.renew_servers_after_new_IP = true;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "NTP client armed against %s (syncs once Wi-Fi is up)", s_ntp_server);
    return ESP_OK;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

const char *time_sync_source(void)
{
    if (!s_synced) return "none";
    return s_via_ntp ? "ntp" : "manual";
}

esp_err_t time_sync_set_manual(int64_t epoch_seconds)
{
    struct timeval tv = { .tv_sec = (time_t)epoch_seconds, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    s_synced = true;
    s_via_ntp = false;
    ESP_LOGI(TAG, "Time set manually to epoch %lld", (long long)epoch_seconds);
    return ESP_OK;
}
