#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

/* esp_sntp_config_t only stores the pointer, not a copy -- keep the server
 * string alive for as long as SNTP might be (re)using it. */
static char s_ntp_server[64];
static volatile bool s_synced = false;
static volatile bool s_via_ntp = false;
/* uint32_t (not time_t/int64_t) so the write from the lwIP task and the read
 * from an httpd worker are each a single atomic 32-bit access. Good until 2106. */
static volatile uint32_t s_last_ntp_epoch = 0;
static bool s_initialized = false;

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    s_via_ntp = true;
    s_last_ntp_epoch = (uint32_t)time(NULL);
    ESP_LOGI(TAG, "Time synchronized via NTP (%s), next re-sync in %d h",
             s_ntp_server, CONFIG_NTP_SYNC_INTERVAL_HOURS);
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

    /* Re-sync once per CONFIG_NTP_SYNC_INTERVAL_HOURS (default 24 h) instead of
     * lwIP's compile-time CONFIG_LWIP_SNTP_UPDATE_DELAY (1 h): 1 request/day
     * instead of 24. Takes effect for the next scheduled request; failed
     * requests are still retried by lwIP's own short back-off, so a missed
     * sync does not wait a whole day. */
    sntp_set_sync_interval((uint32_t)CONFIG_NTP_SYNC_INTERVAL_HOURS * 3600u * 1000u);

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "NTP client armed against %s (syncs once Wi-Fi is up, then every %d h)",
             s_ntp_server, CONFIG_NTP_SYNC_INTERVAL_HOURS);
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

int64_t time_sync_last_ntp_epoch(void)
{
    return (int64_t)s_last_ntp_epoch;
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
