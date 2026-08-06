#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "driver_http.h"

static const char *TAG = "driver_http";

esp_err_t driver_http_set(const plug_t *plug, bool on, char *detail, size_t detail_size)
{
    const char *url = on ? plug->http.on_url : plug->http.off_url;
    if (!url || url[0] == '\0') {
        snprintf(detail, detail_size, "no %s url configured", on ? "on" : "off");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = plug->http.use_post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(detail, detail_size, "client init failed");
        return ESP_FAIL;
    }

    if (plug->http.auth_user[0]) {
        esp_http_client_set_username(client, plug->http.auth_user);
        esp_http_client_set_password(client, plug->http.auth_pass);
        esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s -> %s", url, esp_err_to_name(err));
        snprintf(detail, detail_size, "http error: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(detail, detail_size, "http %d", status);
    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
