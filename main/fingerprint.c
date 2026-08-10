#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "fingerprint.h"

static const char *TAG = "fingerprint";

#define FETCH_BUF_SIZE 2048

/* ---------------------------------------------------------------------- */
/* small HTTP GET helper -- captures the response body (fingerprint.c needs
 * to actually read it, unlike driver_http.c's fire-and-forget on/off calls) */
/* ---------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t buf_size;
    size_t written;
} fetch_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        fetch_ctx_t *ctx = (fetch_ctx_t *)evt->user_data;
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

/* GET url, capture up to body_out_size-1 bytes of the response body (JSON
 * replies from these firmwares are small; HTML index pages get truncated,
 * which is fine since we only substring-search them). Every request sends
 * "Connection: close" -- Tasmota on ESP8266 misbehaves with keep-alive. */
static esp_err_t http_get(const char *url, char *body_out, size_t body_out_size, int *status_out)
{
    fetch_ctx_t ctx = { .buf = body_out, .buf_size = body_out_size, .written = 0 };
    body_out[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_FINGERPRINT_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        /* Fingerprinting only cares whether THIS ip:80 itself answers like a
         * Shelly/Tasmota -- not wherever a 3xx points to. Without this, a
         * plain reverse-proxy/dev box (Traefik, nginx-proxy, etc.) on the
         * scanned host redirects unknown paths like /shelly to its own
         * vhost, esp_http_client happily follows, and the connect then
         * fails trying to resolve that hostname (which typically only
         * exists in the proxy's own local DNS/hosts, not on our LAN) --
         * logged as a confusing esp-tls "couldn't get hostname" error that
         * has nothing to do with the actual target device. */
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    body_out[ctx.written] = '\0';
    if (status_out) *status_out = status;
    return err;
}

/* Case-insensitive substring search -- not relying on strcasestr() being
 * available in every toolchain/newlib config. */
static const char *find_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return NULL;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return p;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Shelly                                                                  */
/* ---------------------------------------------------------------------- */

static void shelly_kind_from_type(const char *type, plug_kind_t *kind)
{
    if (!type) return;
    if (strncmp(type, "SHPLG", 5) == 0) { *kind = PLUG_KIND_PLUG; return; }   /* SHPLG-S, SHPLG2-1, SHPLG-U1 */
    if (strncmp(type, "SHSW", 4) == 0)  { *kind = PLUG_KIND_RELAY; return; }
    if (strncmp(type, "SHDM", 4) == 0)  { *kind = PLUG_KIND_LIGHT; return; }
    if (strncmp(type, "SHBLB", 5) == 0) { *kind = PLUG_KIND_LIGHT; return; }
}

static void shelly_kind_from_app(const char *app, const char *model, plug_kind_t *kind)
{
    if (app) {
        if (strcmp(app, "PlusPlugS") == 0 || strcmp(app, "PlugUS") == 0 || strcmp(app, "PlugUK") == 0 ||
            strcmp(app, "PlugSG3") == 0 || strcmp(app, "PlugMG3") == 0) { *kind = PLUG_KIND_PLUG; return; }
        if (strcmp(app, "Plus1PM") == 0 || strcmp(app, "Plus2PM") == 0) { *kind = PLUG_KIND_RELAY; return; }
    }
    /* Model prefix, not full string -- the same hardware ships under several
     * SKUs (e.g. Plug M Gen3 = S3PL-30110EU and S3PL-30116EU). */
    if (model && strncmp(model, "S3PL-", 5) == 0) { *kind = PLUG_KIND_PLUG; return; }
}

/* Returns true if this host answered as Shelly (any generation). */
static bool try_shelly(const char *ip, plug_fingerprint_t *out)
{
    char url[URL_LEN];
    snprintf(url, sizeof(url), "http://%s/shelly", ip);

    static char body[FETCH_BUF_SIZE];
    int status = 0;
    esp_err_t err = http_get(url, body, sizeof(body), &status);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "%s GET /shelly failed: %s", ip, esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGI(TAG, "%s GET /shelly -> HTTP %d, not Shelly", ip, status);
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGI(TAG, "%s GET /shelly returned non-JSON body, not Shelly", ip);
        return false;
    }

    const cJSON *gen = cJSON_GetObjectItemCaseSensitive(root, "gen");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");   /* Gen1 only */
    const cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app");     /* Gen2+ */
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model"); /* Gen2+ */
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "auth");        /* Gen1 */
    const cJSON *auth_en = cJSON_GetObjectItemCaseSensitive(root, "auth_en");  /* Gen2+ */

    bool is_gen1 = !cJSON_IsNumber(gen);

    if (is_gen1) {
        out->family = PLUG_FAMILY_SHELLY_GEN1;
        if (cJSON_IsString(type) && type->valuestring) {
            snprintf(out->model, sizeof(out->model), "%s", type->valuestring);
            shelly_kind_from_type(type->valuestring, &out->kind);
        }
        out->needs_auth = cJSON_IsTrue(auth);
        snprintf(out->on_url, sizeof(out->on_url), "http://%s/relay/0?turn=on", ip);
        snprintf(out->off_url, sizeof(out->off_url), "http://%s/relay/0?turn=off", ip);
    } else {
        out->family = PLUG_FAMILY_SHELLY_GEN2; /* covers Gen2, Gen3 and newer -- RPC shape is the same */
        const char *app_s = (cJSON_IsString(app) && app->valuestring) ? app->valuestring : NULL;
        const char *model_s = (cJSON_IsString(model) && model->valuestring) ? model->valuestring : NULL;
        if (model_s) snprintf(out->model, sizeof(out->model), "%s", model_s);
        else if (app_s) snprintf(out->model, sizeof(out->model), "%s", app_s);
        shelly_kind_from_app(app_s, model_s, &out->kind);
        out->needs_auth = cJSON_IsTrue(auth_en);
        snprintf(out->on_url, sizeof(out->on_url), "http://%s/rpc/Switch.Set?id=0&on=true", ip);
        snprintf(out->off_url, sizeof(out->off_url), "http://%s/rpc/Switch.Set?id=0&on=false", ip);
    }

    cJSON_Delete(root);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Tasmota                                                                 */
/* ---------------------------------------------------------------------- */

static void tasmota_kind_heuristic(const cJSON *status0_root, plug_kind_t *kind, int *channel_count)
{
    const cJSON *sts = cJSON_GetObjectItemCaseSensitive(status0_root, "StatusSTS");
    if (!sts) return;

    static const char *power_keys[] = { "POWER1", "POWER2", "POWER3", "POWER4" };
    int n = 0;
    for (size_t i = 0; i < sizeof(power_keys) / sizeof(power_keys[0]); i++) {
        if (cJSON_GetObjectItemCaseSensitive(sts, power_keys[i])) n++;
    }
    if (n > 0) {
        *channel_count = n;
        if (*kind == PLUG_KIND_UNKNOWN) *kind = PLUG_KIND_PLUG;
    } else if (cJSON_GetObjectItemCaseSensitive(sts, "POWER")) {
        *channel_count = 1;
        if (*kind == PLUG_KIND_UNKNOWN) *kind = PLUG_KIND_PLUG;
    }

    if (cJSON_GetObjectItemCaseSensitive(sts, "Dimmer") || cJSON_GetObjectItemCaseSensitive(sts, "Color") ||
        cJSON_GetObjectItemCaseSensitive(sts, "CT") || cJSON_GetObjectItemCaseSensitive(sts, "HSBColor")) {
        *kind = PLUG_KIND_LIGHT;
    }

    /* Energy monitoring (StatusSNS.ENERGY.{Voltage,Current,Power}) is a
     * strong "this is a metered plug" signal per the spec. */
    const cJSON *sns = cJSON_GetObjectItemCaseSensitive(status0_root, "StatusSNS");
    if (sns) {
        const cJSON *energy = cJSON_GetObjectItemCaseSensitive(sns, "ENERGY");
        if (energy && (cJSON_GetObjectItemCaseSensitive(energy, "Voltage") ||
                       cJSON_GetObjectItemCaseSensitive(energy, "Power"))) {
            *kind = PLUG_KIND_PLUG;
        }
    }
}

static bool try_tasmota(const char *ip, plug_fingerprint_t *out)
{
    char url[URL_LEN];
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Status%%200", ip);

    static char body[FETCH_BUF_SIZE];
    int status = 0;
    esp_err_t err = http_get(url, body, sizeof(body), &status);

    if (status == 401) {
        out->family = PLUG_FAMILY_TASMOTA;
        out->needs_auth = true;
        return true; /* confirmed family, but can't read anything else without creds */
    }
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "%s GET /cm?cmnd=Status failed: %s", ip, esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGI(TAG, "%s GET /cm?cmnd=Status -> HTTP %d, not Tasmota", ip, status);
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    /* A bare "Status" object with no plausible Tasmota-shaped fields means
     * this almost certainly isn't Tasmota -- don't claim the family. */
    if (!root || !cJSON_GetObjectItemCaseSensitive(root, "Status")) {
        if (root) cJSON_Delete(root);
        ESP_LOGI(TAG, "%s GET /cm?cmnd=Status -> JSON without \"Status\" field, not Tasmota", ip);
        return false;
    }

    out->family = PLUG_FAMILY_TASMOTA;

    int channel_count = 0;
    tasmota_kind_heuristic(root, &out->kind, &channel_count);
    if (channel_count < 1) channel_count = 1;
    cJSON_Delete(root);

    /* Stage 3: template name for a human-readable model, if the device
     * exposes one (empty on stock/unmodified templates). */
    snprintf(url, sizeof(url), "http://%s/cm?cmnd=Template", ip);
    int tstatus = 0;
    if (http_get(url, body, sizeof(body), &tstatus) == ESP_OK && tstatus == 200) {
        cJSON *troot = cJSON_Parse(body);
        if (troot) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(troot, "NAME");
            if (cJSON_IsString(name) && name->valuestring && name->valuestring[0]) {
                snprintf(out->model, sizeof(out->model), "%s", name->valuestring);
            }
            cJSON_Delete(troot);
        }
    }
    if (out->model[0] == '\0') {
        snprintf(out->model, sizeof(out->model), "Tasmota (%d ch)", channel_count);
    }

    /* Single relay: "Power1" and "Power" are equivalent on Tasmota, but the
     * numbered form also works for the (common) single-channel case, so
     * always use it -- one template covers both. */
    snprintf(out->on_url, sizeof(out->on_url), "http://%s/cm?cmnd=Power1%%20On", ip);
    snprintf(out->off_url, sizeof(out->off_url), "http://%s/cm?cmnd=Power1%%20Off", ip);
    return true;
}

/* ---------------------------------------------------------------------- */
/* ESPHome (REST, if the device's own YAML enables web_server + a switch)  */
/* ---------------------------------------------------------------------- */

#if CONFIG_FINGERPRINT_ENABLE_ESPHOME
static bool try_esphome(const char *ip, plug_fingerprint_t *out)
{
    char url[URL_LEN];
    snprintf(url, sizeof(url), "http://%s/", ip);

    static char body[FETCH_BUF_SIZE];
    int status = 0;
    if (http_get(url, body, sizeof(body), &status) != ESP_OK) return false;
    if (status == 401) {
        /* Can't tell if it's ESPHome without a body, but a bare 401 on a
         * still-open :80 is at least worth flagging as "needs auth". */
        return false;
    }
    if (status != 200 || !find_ci(body, "esphome")) return false;

    out->family = PLUG_FAMILY_ESPHOME;
    out->kind = PLUG_KIND_UNKNOWN; /* can't tell plug vs. relay vs. light from the index page alone */
    snprintf(out->model, sizeof(out->model), "ESPHome");

    /* Best-effort: ESPHome's built-in web_server lists entities as links
     * like /switch/<id>/toggle or /switch/<id>/turn_on. Grab the first
     * switch id found -- if there's more than one switch the user will
     * need to double check/adjust the URL manually. */
    const char *p = find_ci(body, "/switch/");
    if (p) {
        p += strlen("/switch/");
        char id[48] = {0};
        size_t n = 0;
        while (p[n] && p[n] != '/' && p[n] != '"' && p[n] != '\'' && n < sizeof(id) - 1) {
            id[n] = p[n];
            n++;
        }
        if (n > 0) {
            snprintf(out->on_url, sizeof(out->on_url), "http://%s/switch/%s/turn_on", ip, id);
            snprintf(out->off_url, sizeof(out->off_url), "http://%s/switch/%s/turn_off", ip, id);
            out->use_post = true;
            out->kind = PLUG_KIND_PLUG; /* best guess -- a switch entity is the closest match we have */
        }
    }
    return true;
}
#endif

/* ---------------------------------------------------------------------- */

esp_err_t fingerprint_probe(const char *ip, plug_fingerprint_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!ip || !ip[0]) return ESP_ERR_INVALID_ARG;

    if (try_shelly(ip, out)) {
        ESP_LOGI(TAG, "%s -> Shelly (%s)", ip, out->model[0] ? out->model : "?");
        return ESP_OK;
    }
    if (try_tasmota(ip, out)) {
        ESP_LOGI(TAG, "%s -> Tasmota (%s)", ip, out->model[0] ? out->model : "?");
        return ESP_OK;
    }
#if CONFIG_FINGERPRINT_ENABLE_ESPHOME
    if (try_esphome(ip, out)) {
        ESP_LOGI(TAG, "%s -> ESPHome", ip);
        return ESP_OK;
    }
#endif
    ESP_LOGD(TAG, "%s -> unrecognized", ip);
    return ESP_OK; /* not an error -- caller just gets family=UNKNOWN */
}

const char *plug_family_label(plug_family_t f)
{
    switch (f) {
        case PLUG_FAMILY_SHELLY_GEN1: return "shelly_gen1";
        case PLUG_FAMILY_SHELLY_GEN2: return "shelly_gen2";
        case PLUG_FAMILY_TASMOTA:     return "tasmota";
        case PLUG_FAMILY_ESPHOME:     return "esphome";
        default:                      return "unknown";
    }
}

const char *plug_kind_label(plug_kind_t k)
{
    switch (k) {
        case PLUG_KIND_PLUG:  return "plug";
        case PLUG_KIND_RELAY: return "relay";
        case PLUG_KIND_LIGHT: return "light";
        case PLUG_KIND_OTHER: return "other";
        default:               return "unknown";
    }
}
