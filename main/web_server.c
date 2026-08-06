#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "web_server.h"
#include "app_types.h"
#include "plug_manager.h"
#include "gpio_inputs.h"
#include "event_log.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";

#define REQ_BODY_MAX 2048

extern const uint8_t web_index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t web_index_html_end[]   asm("_binary_web_index_html_end");
extern const uint8_t web_app_js_start[] asm("_binary_web_app_js_start");
extern const uint8_t web_app_js_end[]   asm("_binary_web_app_js_end");
extern const uint8_t web_style_css_start[] asm("_binary_web_style_css_start");
extern const uint8_t web_style_css_end[]   asm("_binary_web_style_css_end");

/* ---------------------------------------------------------------------- */
/* helpers                                                                */
/* ---------------------------------------------------------------------- */

static void set_status(httpd_req_t *req, int code)
{
    switch (code) {
        case 200: httpd_resp_set_status(req, "200 OK"); break;
        case 201: httpd_resp_set_status(req, "201 Created"); break;
        case 400: httpd_resp_set_status(req, "400 Bad Request"); break;
        case 404: httpd_resp_set_status(req, "404 Not Found"); break;
        case 409: httpd_resp_set_status(req, "409 Conflict"); break;
        default:  httpd_resp_set_status(req, "500 Internal Server Error"); break;
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int code)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    set_status(req, code);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, code);
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= buf_size) {
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static void jget_str(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static bool jget_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static int jget_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static const char *input_label(input_id_t id)
{
    switch (id) {
        case INPUT_AC1: return "AC1";
        case INPUT_DC1: return "DC1";
        case INPUT_DC2: return "DC2";
        default: return "NONE";
    }
}

static input_id_t input_from_label(const char *s)
{
    if (!s) return INPUT_NONE;
    if (strcmp(s, "AC1") == 0) return INPUT_AC1;
    if (strcmp(s, "DC1") == 0) return INPUT_DC1;
    if (strcmp(s, "DC2") == 0) return INPUT_DC2;
    return INPUT_NONE;
}

static const char *driver_label(driver_type_t d)
{
    switch (d) {
        case DRIVER_HTTP: return "http";
        case DRIVER_MQTT: return "mqtt";
        case DRIVER_ZIGBEE: return "zigbee";
        default: return "http";
    }
}

static driver_type_t driver_from_label(const char *s)
{
    if (!s) return DRIVER_HTTP;
    if (strcmp(s, "mqtt") == 0) return DRIVER_MQTT;
    if (strcmp(s, "zigbee") == 0) return DRIVER_ZIGBEE;
    return DRIVER_HTTP;
}

static cJSON *plug_to_json(const plug_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddBoolToObject(o, "enabled", p->enabled);
    cJSON_AddStringToObject(o, "input", input_label(p->input));
    cJSON_AddBoolToObject(o, "invert", p->invert);
    cJSON_AddStringToObject(o, "driver", driver_label(p->driver));

    cJSON *http = cJSON_CreateObject();
    cJSON_AddStringToObject(http, "on_url", p->http.on_url);
    cJSON_AddStringToObject(http, "off_url", p->http.off_url);
    cJSON_AddStringToObject(http, "status_url", p->http.status_url);
    cJSON_AddBoolToObject(http, "use_post", p->http.use_post);
    cJSON_AddStringToObject(http, "auth_user", p->http.auth_user);
    cJSON_AddStringToObject(http, "auth_pass", p->http.auth_pass);
    cJSON_AddItemToObject(o, "http", http);

    cJSON *mqtt = cJSON_CreateObject();
    cJSON_AddStringToObject(mqtt, "cmd_topic", p->mqtt.cmd_topic);
    cJSON_AddStringToObject(mqtt, "payload_on", p->mqtt.payload_on);
    cJSON_AddStringToObject(mqtt, "payload_off", p->mqtt.payload_off);
    cJSON_AddStringToObject(mqtt, "state_topic", p->mqtt.state_topic);
    cJSON_AddNumberToObject(mqtt, "qos", p->mqtt.qos);
    cJSON_AddBoolToObject(mqtt, "retain", p->mqtt.retain);
    cJSON_AddItemToObject(o, "mqtt", mqtt);

    cJSON *zb = cJSON_CreateObject();
    cJSON_AddStringToObject(zb, "note", p->zigbee.note);
    cJSON_AddBoolToObject(zb, "implemented", false);
    cJSON_AddItemToObject(o, "zigbee", zb);

    cJSON_AddBoolToObject(o, "last_state", p->last_state);
    cJSON_AddBoolToObject(o, "last_cmd_ok", p->last_cmd_ok);
    cJSON_AddNumberToObject(o, "last_change_us", (double)p->last_change_us);

    return o;
}

static void plug_from_json(const cJSON *o, plug_t *p)
{
    memset(p, 0, sizeof(*p));
    jget_str(o, "name", p->name, sizeof(p->name));
    p->enabled = jget_bool(o, "enabled", true);

    char input_str[8];
    jget_str(o, "input", input_str, sizeof(input_str));
    p->input = input_from_label(input_str);
    p->invert = jget_bool(o, "invert", false);

    char driver_str[8];
    jget_str(o, "driver", driver_str, sizeof(driver_str));
    p->driver = driver_from_label(driver_str);

    const cJSON *http = cJSON_GetObjectItemCaseSensitive(o, "http");
    if (http) {
        jget_str(http, "on_url", p->http.on_url, sizeof(p->http.on_url));
        jget_str(http, "off_url", p->http.off_url, sizeof(p->http.off_url));
        jget_str(http, "status_url", p->http.status_url, sizeof(p->http.status_url));
        p->http.use_post = jget_bool(http, "use_post", false);
        jget_str(http, "auth_user", p->http.auth_user, sizeof(p->http.auth_user));
        jget_str(http, "auth_pass", p->http.auth_pass, sizeof(p->http.auth_pass));
    }

    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(o, "mqtt");
    if (mqtt) {
        jget_str(mqtt, "cmd_topic", p->mqtt.cmd_topic, sizeof(p->mqtt.cmd_topic));
        jget_str(mqtt, "payload_on", p->mqtt.payload_on, sizeof(p->mqtt.payload_on));
        jget_str(mqtt, "payload_off", p->mqtt.payload_off, sizeof(p->mqtt.payload_off));
        jget_str(mqtt, "state_topic", p->mqtt.state_topic, sizeof(p->mqtt.state_topic));
        p->mqtt.qos = jget_int(mqtt, "qos", 0);
        p->mqtt.retain = jget_bool(mqtt, "retain", false);
    }

    const cJSON *zb = cJSON_GetObjectItemCaseSensitive(o, "zigbee");
    if (zb) {
        jget_str(zb, "note", p->zigbee.note, sizeof(p->zigbee.note));
    }
}

/* Parses the numeric id segment right after prefix "/api/plugs/", stopping
 * at '/' or end of string. Returns 0 if not a valid number. */
static uint32_t parse_id_after(const char *uri, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return 0;
    return (uint32_t)strtoul(uri + plen, NULL, 10);
}

static bool uri_ends_with(const char *uri, const char *suffix)
{
    size_t ul = strlen(uri), sl = strlen(suffix);
    if (ul < sl) return false;
    return strcmp(uri + (ul - sl), suffix) == 0;
}

/* ---------------------------------------------------------------------- */
/* static file handlers                                                   */
/* ---------------------------------------------------------------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)web_index_html_start,
                            web_index_html_end - web_index_html_start);
}

static esp_err_t h_app_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, (const char *)web_app_js_start,
                            web_app_js_end - web_app_js_start);
}

static esp_err_t h_style_css(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)web_style_css_start,
                            web_style_css_end - web_style_css_start);
}

/* ---------------------------------------------------------------------- */
/* /api/plugs                                                              */
/* ---------------------------------------------------------------------- */

static esp_err_t h_plugs_get(httpd_req_t *req)
{
    plug_t plugs[MAX_PLUGS];
    size_t n = plug_manager_get_all(plugs, MAX_PLUGS);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, plug_to_json(&plugs[i]));
    }
    return send_json(req, arr, 200);
}

static esp_err_t h_plugs_post(httpd_req_t *req)
{
    char body[REQ_BODY_MAX];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error(req, 400, "body too large or missing");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid json");

    plug_t p;
    plug_from_json(root, &p);
    cJSON_Delete(root);

    if (p.name[0] == '\0') return send_error(req, 400, "name required");

    esp_err_t err = plug_manager_add(&p);
    if (err != ESP_OK) return send_error(req, 409, "plug limit reached or storage error");

    return send_json(req, plug_to_json(&p), 201);
}

/* Handles PUT /api/plugs/{id} and DELETE /api/plugs/{id} and
 * POST /api/plugs/{id}/action. */
static esp_err_t h_plugs_wildcard(httpd_req_t *req)
{
    uint32_t id = parse_id_after(req->uri, "/api/plugs/");
    if (id == 0) return send_error(req, 400, "invalid plug id");

    if (req->method == HTTP_PUT) {
        char body[REQ_BODY_MAX];
        if (recv_body(req, body, sizeof(body)) != ESP_OK) {
            return send_error(req, 400, "body too large or missing");
        }
        cJSON *root = cJSON_Parse(body);
        if (!root) return send_error(req, 400, "invalid json");
        plug_t p;
        plug_from_json(root, &p);
        cJSON_Delete(root);

        esp_err_t err = plug_manager_update(id, &p);
        if (err == ESP_ERR_NOT_FOUND) return send_error(req, 404, "plug not found");
        if (err != ESP_OK) return send_error(req, 500, "failed to save");

        plug_t updated;
        plug_manager_get(id, &updated);
        return send_json(req, plug_to_json(&updated), 200);

    } else if (req->method == HTTP_DELETE) {
        esp_err_t err = plug_manager_delete(id);
        if (err == ESP_ERR_NOT_FOUND) return send_error(req, 404, "plug not found");
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        return send_json(req, root, 200);

    } else if (req->method == HTTP_POST && uri_ends_with(req->uri, "/action")) {
        char body[256];
        if (recv_body(req, body, sizeof(body)) != ESP_OK) {
            return send_error(req, 400, "body too large or missing");
        }
        cJSON *root = cJSON_Parse(body);
        if (!root) return send_error(req, 400, "invalid json");
        char action[8];
        jget_str(root, "action", action, sizeof(action));
        cJSON_Delete(root);

        bool on;
        if (strcmp(action, "on") == 0) on = true;
        else if (strcmp(action, "off") == 0) on = false;
        else return send_error(req, 400, "action must be \"on\" or \"off\"");

        char detail[96] = {0};
        bool ok = false;
        esp_err_t err = plug_manager_test_action(id, on, detail, sizeof(detail), &ok);
        if (err == ESP_ERR_NOT_FOUND) return send_error(req, 404, "plug not found");

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", ok);
        cJSON_AddStringToObject(resp, "detail", detail);
        return send_json(req, resp, 200);
    }

    return send_error(req, 404, "not found");
}

/* ---------------------------------------------------------------------- */
/* /api/settings                                                           */
/* ---------------------------------------------------------------------- */

static cJSON *settings_to_json(const system_settings_t *s)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *inputs = cJSON_CreateArray();
    for (int i = 0; i < INPUT_COUNT_PHYSICAL; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", s->inputs[i].label);
        cJSON_AddNumberToObject(o, "gpio", s->inputs[i].gpio);
        cJSON_AddBoolToObject(o, "enabled", s->inputs[i].enabled);
        cJSON_AddItemToArray(inputs, o);
    }
    cJSON_AddItemToObject(root, "inputs", inputs);
    cJSON_AddNumberToObject(root, "debounce_ms", s->debounce_ms);
    cJSON_AddNumberToObject(root, "poll_interval_ms", s->poll_interval_ms);
    cJSON_AddStringToObject(root, "mqtt_uri", s->mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_user", s->mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_pass", s->mqtt_pass);
    cJSON_AddStringToObject(root, "mqtt_client_id", s->mqtt_client_id);
    cJSON_AddNumberToObject(root, "log_max_file_size", s->log_max_file_size);
    cJSON_AddNumberToObject(root, "log_max_files", s->log_max_files);
    return root;
}

static void settings_from_json(const cJSON *root, system_settings_t *s)
{
    /* start from current values so partial updates don't wipe fields */
    plug_manager_get_settings(s);

    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    if (cJSON_IsArray(inputs)) {
        int i = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, inputs) {
            if (i >= INPUT_COUNT_PHYSICAL) break;
            s->inputs[i].gpio = jget_int(item, "gpio", s->inputs[i].gpio);
            s->inputs[i].enabled = jget_bool(item, "enabled", s->inputs[i].enabled);
            i++;
        }
    }
    s->debounce_ms = (uint32_t)jget_int(root, "debounce_ms", (int)s->debounce_ms);
    s->poll_interval_ms = (uint32_t)jget_int(root, "poll_interval_ms", (int)s->poll_interval_ms);
    jget_str(root, "mqtt_uri", s->mqtt_uri, sizeof(s->mqtt_uri));
    jget_str(root, "mqtt_user", s->mqtt_user, sizeof(s->mqtt_user));
    jget_str(root, "mqtt_pass", s->mqtt_pass, sizeof(s->mqtt_pass));
    jget_str(root, "mqtt_client_id", s->mqtt_client_id, sizeof(s->mqtt_client_id));
    s->log_max_file_size = (uint32_t)jget_int(root, "log_max_file_size", (int)s->log_max_file_size);
    s->log_max_files = (uint32_t)jget_int(root, "log_max_files", (int)s->log_max_files);
}

static esp_err_t h_settings_get(httpd_req_t *req)
{
    system_settings_t s;
    plug_manager_get_settings(&s);
    return send_json(req, settings_to_json(&s), 200);
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    char body[REQ_BODY_MAX];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error(req, 400, "body too large or missing");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid json");

    system_settings_t s;
    settings_from_json(root, &s);
    cJSON_Delete(root);

    esp_err_t err = plug_manager_update_settings(&s);
    if (err != ESP_OK) return send_error(req, 500, "failed to save settings");

    return send_json(req, settings_to_json(&s), 200);
}

/* ---------------------------------------------------------------------- */
/* /api/log                                                                 */
/* ---------------------------------------------------------------------- */

static esp_err_t h_log_get(httpd_req_t *req)
{
    int limit = 200;
    char q[32];
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(q, "limit", val, sizeof(val)) == ESP_OK) {
            int v = atoi(val);
            if (v > 0 && v <= 1000) limit = v;
        }
    }

    static char out[8192];
    esp_err_t err = event_log_read_recent(out, sizeof(out), limit);
    if (err != ESP_OK) return send_error(req, 500, "failed to read log");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t h_log_clear(httpd_req_t *req)
{
    event_log_clear();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root, 200);
}

/* ---------------------------------------------------------------------- */
/* /api/status, /api/wifi                                                   */
/* ---------------------------------------------------------------------- */

static esp_err_t h_status_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *inputs = cJSON_CreateArray();
    static const input_id_t ids[] = { INPUT_AC1, INPUT_DC1, INPUT_DC2 };
    static const char *labels[] = { "AC1", "DC1", "DC2" };
    for (int i = 0; i < 3; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", labels[i]);
        cJSON_AddBoolToObject(o, "enabled", gpio_inputs_is_enabled(ids[i]));
        cJSON_AddBoolToObject(o, "level", gpio_inputs_get_level(ids[i]));
        cJSON_AddItemToArray(inputs, o);
    }
    cJSON_AddItemToObject(root, "inputs", inputs);

    char sta_ip[16], ap_ip[16], ssid[33];
    wifi_manager_get_status(sta_ip, sizeof(sta_ip), ap_ip, sizeof(ap_ip), ssid, sizeof(ssid));
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "sta_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(wifi, "sta_ip", sta_ip);
    cJSON_AddStringToObject(wifi, "ap_ip", ap_ip);
    cJSON_AddStringToObject(wifi, "ssid", ssid);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    return send_json(req, root, 200);
}

static esp_err_t h_wifi_get(httpd_req_t *req)
{
    char sta_ip[16], ap_ip[16], ssid[33];
    wifi_manager_get_status(sta_ip, sizeof(sta_ip), ap_ip, sizeof(ap_ip), ssid, sizeof(ssid));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    cJSON_AddStringToObject(root, "ap_ip", ap_ip);
    return send_json(req, root, 200);
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    char body[512];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error(req, 400, "body too large or missing");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) return send_error(req, 400, "invalid json");

    char ssid[33], pass[65];
    jget_str(root, "ssid", ssid, sizeof(ssid));
    jget_str(root, "password", pass, sizeof(pass));
    cJSON_Delete(root);

    if (ssid[0] == '\0') return send_error(req, 400, "ssid required");

    esp_err_t err = wifi_manager_set_credentials(ssid, pass);
    if (err != ESP_OK) return send_error(req, 400, "invalid credentials");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp, 200);
}

/* ---------------------------------------------------------------------- */

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEB_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,    .handler = h_index },
        { .uri = "/index.html",    .method = HTTP_GET,    .handler = h_index },
        { .uri = "/app.js",        .method = HTTP_GET,    .handler = h_app_js },
        { .uri = "/style.css",     .method = HTTP_GET,    .handler = h_style_css },
        { .uri = "/api/plugs",     .method = HTTP_GET,    .handler = h_plugs_get },
        { .uri = "/api/plugs",     .method = HTTP_POST,   .handler = h_plugs_post },
        { .uri = "/api/plugs/*",   .method = HTTP_PUT,    .handler = h_plugs_wildcard },
        { .uri = "/api/plugs/*",   .method = HTTP_DELETE, .handler = h_plugs_wildcard },
        { .uri = "/api/plugs/*",   .method = HTTP_POST,   .handler = h_plugs_wildcard },
        { .uri = "/api/settings",  .method = HTTP_GET,    .handler = h_settings_get },
        { .uri = "/api/settings",  .method = HTTP_POST,   .handler = h_settings_post },
        { .uri = "/api/log",       .method = HTTP_GET,    .handler = h_log_get },
        { .uri = "/api/log/clear", .method = HTTP_POST,   .handler = h_log_clear },
        { .uri = "/api/status",    .method = HTTP_GET,    .handler = h_status_get },
        { .uri = "/api/wifi",      .method = HTTP_GET,    .handler = h_wifi_get },
        { .uri = "/api/wifi",      .method = HTTP_POST,   .handler = h_wifi_post },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
