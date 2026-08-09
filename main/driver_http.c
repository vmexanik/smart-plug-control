#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "driver_http.h"

static const char *TAG = "driver_http";

/* --- status query (driver_http_get_status) --------------------------- */

/* Pulls "host[:port]" out of a "http://host[:port]/path..." URL -- used to
 * build the family-specific status URL from the already-known on_url
 * rather than needing a separately stored host/IP field. */
static bool extract_host(const char *url, char *out, size_t out_size)
{
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

typedef struct {
    char *buf;
    size_t buf_size;
    size_t written;
} status_fetch_ctx_t;

static esp_err_t status_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        status_fetch_ctx_t *ctx = (status_fetch_ctx_t *)evt->user_data;
        if (ctx && ctx->buf && evt->data && evt->data_len > 0) {
            size_t space = (ctx->buf_size > ctx->written + 1) ? (ctx->buf_size - 1 - ctx->written) : 0;
            size_t n = (size_t)evt->data_len < space ? (size_t)evt->data_len : space;
            if (n > 0) {
                memcpy(ctx->buf + ctx->written, evt->data, n);
                ctx->written += n;
            }
        }
    }
    return ESP_OK;
}

esp_err_t driver_http_get_status(const plug_t *plug, bool *out_on)
{
    if (!plug || !out_on) return ESP_ERR_INVALID_ARG;
    if (plug->http.family == PLUG_FAMILY_UNKNOWN) return ESP_ERR_NOT_SUPPORTED;

    char host[64];
    if (!extract_host(plug->http.on_url, host, sizeof(host))) return ESP_ERR_NOT_SUPPORTED;

    char url[URL_LEN];
    switch (plug->http.family) {
        case PLUG_FAMILY_SHELLY_GEN1:
            snprintf(url, sizeof(url), "http://%s/relay/0", host);
            break;
        case PLUG_FAMILY_SHELLY_GEN2:
            snprintf(url, sizeof(url), "http://%s/rpc/Switch.GetStatus?id=0", host);
            break;
        case PLUG_FAMILY_TASMOTA:
            snprintf(url, sizeof(url), "http://%s/cm?cmnd=Power1", host);
            break;
        default:
            /* ESPHome: response shape depends entirely on the device's own
             * YAML (entity id, whether JSON is even served without an
             * Accept header) -- not reliable enough to guess at. */
            return ESP_ERR_NOT_SUPPORTED;
    }

    char body[512];
    status_fetch_ctx_t ctx = { .buf = body, .buf_size = sizeof(body), .written = 0 };
    body[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 3000,
        .event_handler = status_event_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    if (plug->http.auth_user[0]) {
        esp_http_client_set_username(client, plug->http.auth_user);
        esp_http_client_set_password(client, plug->http.auth_pass);
        esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    body[ctx.written] = '\0';

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status query %s failed: %s", url, esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "status query %s -> HTTP %d", url, status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_FAIL;

    bool ok = false;
    switch (plug->http.family) {
        case PLUG_FAMILY_SHELLY_GEN1: {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "ison");
            if (cJSON_IsBool(v)) { *out_on = cJSON_IsTrue(v); ok = true; }
            break;
        }
        case PLUG_FAMILY_SHELLY_GEN2: {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "output");
            if (cJSON_IsBool(v)) { *out_on = cJSON_IsTrue(v); ok = true; }
            break;
        }
        case PLUG_FAMILY_TASMOTA: {
            /* Single-relay devices have answered a "Power1" query under
             * either key depending on firmware version -- check both. */
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "POWER1");
            if (!v) v = cJSON_GetObjectItemCaseSensitive(root, "POWER");
            if (cJSON_IsString(v) && v->valuestring) {
                *out_on = (strcmp(v->valuestring, "ON") == 0);
                ok = true;
            }
            break;
        }
        default:
            break;
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

/* --- power metrics query (driver_http_get_metrics) -------------------- */

esp_err_t driver_http_get_metrics(const plug_t *plug, plug_metrics_t *out)
{
    if (!plug || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (plug->http.family == PLUG_FAMILY_UNKNOWN) return ESP_ERR_NOT_SUPPORTED;

    char host[64];
    if (!extract_host(plug->http.on_url, host, sizeof(host))) return ESP_ERR_NOT_SUPPORTED;

    char url[URL_LEN];
    switch (plug->http.family) {
        case PLUG_FAMILY_SHELLY_GEN1:
            snprintf(url, sizeof(url), "http://%s/status", host);
            break;
        case PLUG_FAMILY_SHELLY_GEN2:
            snprintf(url, sizeof(url), "http://%s/rpc/Switch.GetStatus?id=0", host);
            break;
        case PLUG_FAMILY_TASMOTA:
            snprintf(url, sizeof(url), "http://%s/cm?cmnd=Status%%208", host);
            break;
        default:
            /* ESPHome: same "shape depends on the device's own YAML" reasoning
             * as driver_http_get_status(). */
            return ESP_ERR_NOT_SUPPORTED;
    }

    /* static: this function runs on plug_manager.c's metrics_poll_task,
     * which -- like every other esp_http_client-calling task in this
     * project -- is sized just for the client itself; a 1KB buffer on top
     * of that stack is exactly the kind of thing that caused the S3
     * stack-overflow panic documented in README, so keep it off the stack. */
    static char body[1024];
    status_fetch_ctx_t ctx = { .buf = body, .buf_size = sizeof(body), .written = 0 };
    body[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 3000,
        .event_handler = status_event_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    if (plug->http.auth_user[0]) {
        esp_http_client_set_username(client, plug->http.auth_user);
        esp_http_client_set_password(client, plug->http.auth_pass);
        esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    body[ctx.written] = '\0';

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "metrics query %s failed: %s", url, esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "metrics query %s -> HTTP %d", url, status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_FAIL;

    switch (plug->http.family) {
        case PLUG_FAMILY_SHELLY_GEN1: {
            /* Shelly Plug/PlugS: {"meters":[{"power":34.24,"is_valid":true,...}]}
             * -- no voltage/current on Gen1 metering, only power. */
            const cJSON *meters = cJSON_GetObjectItemCaseSensitive(root, "meters");
            /* The vendored cJSON.h (see its header comment) only implements
             * a subset of the real cJSON API -- no cJSON_GetArraySize()/
             * cJSON_GetArrayItem(), so index into the array the same way
             * object members are walked: array elements are a child/next
             * linked list, ->child is element 0. */
            if (cJSON_IsArray(meters) && meters->child) {
                const cJSON *m0 = meters->child;
                const cJSON *power = cJSON_GetObjectItemCaseSensitive(m0, "power");
                if (cJSON_IsNumber(power)) {
                    out->power_w = (float)power->valuedouble;
                    out->valid = true;
                }
            }
            break;
        }
        case PLUG_FAMILY_SHELLY_GEN2: {
            /* Switch.GetStatus (verified against shelly-api-docs.shelly.cloud/
             * gen2/ComponentsAndServices/Switch): apower/voltage/current are
             * only present ("shown if applicable") on metering-capable
             * devices, e.g. Plus/Pro Plug S -- absent on a plain switch. */
            const cJSON *apower = cJSON_GetObjectItemCaseSensitive(root, "apower");
            const cJSON *voltage = cJSON_GetObjectItemCaseSensitive(root, "voltage");
            const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
            if (cJSON_IsNumber(apower)) {
                out->power_w = (float)apower->valuedouble;
                out->valid = true;
            }
            if (cJSON_IsNumber(voltage)) out->voltage_v = (float)voltage->valuedouble;
            if (cJSON_IsNumber(current)) out->current_a = (float)current->valuedouble;
            break;
        }
        case PLUG_FAMILY_TASMOTA: {
            /* Status 8 -> {"StatusSNS":{"ENERGY":{"Voltage":..,"Current":..,
             * "Power":..}}}; same field names fingerprint.c's kind heuristic
             * already keys off of (StatusSNS.ENERGY.{Voltage,Power}), just a
             * different status level (0 there, 8 here). Absent entirely on
             * non-metered Tasmota devices. */
            const cJSON *sns = cJSON_GetObjectItemCaseSensitive(root, "StatusSNS");
            const cJSON *energy = sns ? cJSON_GetObjectItemCaseSensitive(sns, "ENERGY") : NULL;
            if (energy) {
                const cJSON *power = cJSON_GetObjectItemCaseSensitive(energy, "Power");
                const cJSON *voltage = cJSON_GetObjectItemCaseSensitive(energy, "Voltage");
                const cJSON *current = cJSON_GetObjectItemCaseSensitive(energy, "Current");
                if (cJSON_IsNumber(power)) {
                    out->power_w = (float)power->valuedouble;
                    out->valid = true;
                }
                if (cJSON_IsNumber(voltage)) out->voltage_v = (float)voltage->valuedouble;
                if (cJSON_IsNumber(current)) out->current_a = (float)current->valuedouble;
            }
            break;
        }
        default:
            break;
    }
    cJSON_Delete(root);
    return ESP_OK; /* OK regardless of ->valid -- device answered, this is what it said */
}

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
