#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "storage.h"

static const char *TAG = "storage";

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_MOUNT_POINT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount/format LittleFS (%s)", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: %d/%d bytes used", (int)used, (int)total);
    }
    return ESP_OK;
}

void storage_force_fixed_inputs(system_settings_t *settings)
{
    settings->inputs[0].id = INPUT_AC1;
    strncpy(settings->inputs[0].label, "AC1", sizeof(settings->inputs[0].label) - 1);
    settings->inputs[0].label[sizeof(settings->inputs[0].label) - 1] = '\0';
    /* No physical GPIO for AC1 on this board revision -- see input_id_t's
     * comment in app_types.h. -1 makes gpio_inputs.c's poll loop and
     * configure_gpio_input() skip it entirely (both already treat gpio < 0
     * as "nothing to configure/poll here"); its actual trigger level comes
     * from ac_monitor.c via plug_manager_on_ac_mon_change() instead. */
    settings->inputs[0].gpio = -1;

    settings->inputs[1].id = INPUT_DC1;
    strncpy(settings->inputs[1].label, "DC1", sizeof(settings->inputs[1].label) - 1);
    settings->inputs[1].label[sizeof(settings->inputs[1].label) - 1] = '\0';
    settings->inputs[1].gpio = CONFIG_INPUT_DC1_GPIO_DEFAULT;

    settings->inputs[2].id = INPUT_DC2;
    strncpy(settings->inputs[2].label, "DC2", sizeof(settings->inputs[2].label) - 1);
    settings->inputs[2].label[sizeof(settings->inputs[2].label) - 1] = '\0';
    settings->inputs[2].gpio = CONFIG_INPUT_DC2_GPIO_DEFAULT;

    /* Poll interval is a physical-timing constant same as the GPIO pins
     * above -- fixed by Kconfig, not web-editable (see Kconfig.projbuild). */
    settings->poll_interval_ms = CONFIG_INPUT_POLL_INTERVAL_MS_DEFAULT;
}

void storage_default_config(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->plug_count = 0;

    storage_force_fixed_inputs(&cfg->settings); /* also sets poll_interval_ms from Kconfig */
    cfg->settings.inputs[0].enabled = true;
    cfg->settings.inputs[1].enabled = false;
    cfg->settings.inputs[2].enabled = false;

    cfg->settings.mqtt_uri[0] = '\0';
    strncpy(cfg->settings.mqtt_client_id, "smart-plug-hub", sizeof(cfg->settings.mqtt_client_id) - 1);

    /* Always active (no enable/disable setting -- see ac_mon_settings_t's
     * comment in app_types.h). Band is a generic +-10%/+-2% starting point
     * around 230V/50Hz; meant to be tuned per-installation from the web
     * UI, not treated as a real spec. No mains (or an unplugged ZMPT101B)
     * reads as 0 V, i.e. out of band: an AC1-bound plug stays off then. */
    cfg->settings.ac_mon.v_min = 198;
    cfg->settings.ac_mon.v_max = 253;
    cfg->settings.ac_mon.f_min = 49.5f;
    cfg->settings.ac_mon.f_max = 50.5f;
    cfg->settings.ac_mon.cal_k = 0.0f; /* 0 = Kconfig default, see ac_mon_settings_t */
}

/* ---- JSON <-> struct ---- */

static void plug_to_json(const plug_t *p, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddBoolToObject(o, "enabled", p->enabled);
    cJSON_AddNumberToObject(o, "input", (int)p->input);
    cJSON_AddBoolToObject(o, "invert", p->invert);
    cJSON_AddNumberToObject(o, "on_delay_s", p->on_delay_s);
    cJSON_AddNumberToObject(o, "driver", (int)p->driver);

    cJSON *http = cJSON_CreateObject();
    cJSON_AddStringToObject(http, "on_url", p->http.on_url);
    cJSON_AddStringToObject(http, "off_url", p->http.off_url);
    cJSON_AddStringToObject(http, "status_url", p->http.status_url);
    cJSON_AddBoolToObject(http, "use_post", p->http.use_post);
    cJSON_AddStringToObject(http, "auth_user", p->http.auth_user);
    cJSON_AddStringToObject(http, "auth_pass", p->http.auth_pass);
    cJSON_AddNumberToObject(http, "family", (int)p->http.family);
    cJSON_AddNumberToObject(http, "kind", (int)p->http.kind);
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
    cJSON_AddItemToObject(o, "zigbee", zb);

    cJSON_AddItemToArray(arr, o);
}

static void json_get_str(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static bool json_get_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static float json_get_float(const cJSON *obj, const char *key, float def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void plug_from_json(const cJSON *o, plug_t *p)
{
    memset(p, 0, sizeof(*p));
    p->id = (uint32_t)json_get_int(o, "id", 0);
    json_get_str(o, "name", p->name, sizeof(p->name));
    p->enabled = json_get_bool(o, "enabled", true);
    p->input = (input_id_t)json_get_int(o, "input", INPUT_NONE);
    p->invert = json_get_bool(o, "invert", false);
    int od = json_get_int(o, "on_delay_s", 0);
    if (od < 0) od = 0;
    if (od > PLUG_ON_DELAY_S_MAX) od = PLUG_ON_DELAY_S_MAX;
    p->on_delay_s = (uint32_t)od;
    p->driver = (driver_type_t)json_get_int(o, "driver", DRIVER_HTTP);

    const cJSON *http = cJSON_GetObjectItemCaseSensitive(o, "http");
    if (http) {
        json_get_str(http, "on_url", p->http.on_url, sizeof(p->http.on_url));
        json_get_str(http, "off_url", p->http.off_url, sizeof(p->http.off_url));
        json_get_str(http, "status_url", p->http.status_url, sizeof(p->http.status_url));
        p->http.use_post = json_get_bool(http, "use_post", false);
        json_get_str(http, "auth_user", p->http.auth_user, sizeof(p->http.auth_user));
        json_get_str(http, "auth_pass", p->http.auth_pass, sizeof(p->http.auth_pass));
        p->http.family = (plug_family_t)json_get_int(http, "family", PLUG_FAMILY_UNKNOWN);
        p->http.kind = (plug_kind_t)json_get_int(http, "kind", PLUG_KIND_UNKNOWN);
    }

    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(o, "mqtt");
    if (mqtt) {
        json_get_str(mqtt, "cmd_topic", p->mqtt.cmd_topic, sizeof(p->mqtt.cmd_topic));
        json_get_str(mqtt, "payload_on", p->mqtt.payload_on, sizeof(p->mqtt.payload_on));
        json_get_str(mqtt, "payload_off", p->mqtt.payload_off, sizeof(p->mqtt.payload_off));
        json_get_str(mqtt, "state_topic", p->mqtt.state_topic, sizeof(p->mqtt.state_topic));
        p->mqtt.qos = json_get_int(mqtt, "qos", 0);
        p->mqtt.retain = json_get_bool(mqtt, "retain", false);
    }

    const cJSON *zb = cJSON_GetObjectItemCaseSensitive(o, "zigbee");
    if (zb) {
        json_get_str(zb, "note", p->zigbee.note, sizeof(p->zigbee.note));
    }
}

static cJSON *settings_to_json(const system_settings_t *s)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *inputs = cJSON_CreateArray();
    for (int i = 0; i < INPUT_COUNT_PHYSICAL; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (int)s->inputs[i].id);
        cJSON_AddStringToObject(o, "label", s->inputs[i].label);
        cJSON_AddNumberToObject(o, "gpio", s->inputs[i].gpio);
        cJSON_AddBoolToObject(o, "enabled", s->inputs[i].enabled);
        cJSON_AddItemToArray(inputs, o);
    }
    cJSON_AddItemToObject(root, "inputs", inputs);

    cJSON_AddNumberToObject(root, "poll_interval_ms", s->poll_interval_ms);
    cJSON_AddStringToObject(root, "mqtt_uri", s->mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_user", s->mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_pass", s->mqtt_pass);
    cJSON_AddStringToObject(root, "mqtt_client_id", s->mqtt_client_id);

    cJSON *ac_mon = cJSON_CreateObject();
    cJSON_AddNumberToObject(ac_mon, "v_min", s->ac_mon.v_min);
    cJSON_AddNumberToObject(ac_mon, "v_max", s->ac_mon.v_max);
    cJSON_AddNumberToObject(ac_mon, "f_min", (double)s->ac_mon.f_min);
    cJSON_AddNumberToObject(ac_mon, "f_max", (double)s->ac_mon.f_max);
    cJSON_AddNumberToObject(ac_mon, "cal_k", (double)s->ac_mon.cal_k);
    cJSON_AddItemToObject(root, "ac_mon", ac_mon);

    return root;
}

static void settings_from_json(const cJSON *root, system_settings_t *s, const system_settings_t *defaults)
{
    *s = *defaults;

    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    if (cJSON_IsArray(inputs)) {
        int i = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, inputs) {
            if (i >= INPUT_COUNT_PHYSICAL) break;
            s->inputs[i].id = (input_id_t)json_get_int(item, "id", s->inputs[i].id);
            json_get_str(item, "label", s->inputs[i].label, sizeof(s->inputs[i].label));
            /* gpio is intentionally NOT read from JSON -- it is fixed by
             * Kconfig (see storage_force_fixed_inputs() above), never
             * editable via stored config or the web UI. */
            s->inputs[i].enabled = json_get_bool(item, "enabled", s->inputs[i].enabled);
            i++;
        }
    }

    /* poll_interval_ms is intentionally NOT read from JSON --
     * fixed by Kconfig (see storage_force_fixed_inputs()), same as gpio
     * above; still written out by settings_to_json() below purely so
     * GET /api/settings can display the effective values. */
    json_get_str(root, "mqtt_uri", s->mqtt_uri, sizeof(s->mqtt_uri));
    json_get_str(root, "mqtt_user", s->mqtt_user, sizeof(s->mqtt_user));
    json_get_str(root, "mqtt_pass", s->mqtt_pass, sizeof(s->mqtt_pass));
    json_get_str(root, "mqtt_client_id", s->mqtt_client_id, sizeof(s->mqtt_client_id));

    const cJSON *ac_mon = cJSON_GetObjectItemCaseSensitive(root, "ac_mon");
    if (ac_mon) {
        s->ac_mon.v_min = json_get_int(ac_mon, "v_min", s->ac_mon.v_min);
        s->ac_mon.v_max = json_get_int(ac_mon, "v_max", s->ac_mon.v_max);
        s->ac_mon.f_min = json_get_float(ac_mon, "f_min", s->ac_mon.f_min);
        s->ac_mon.f_max = json_get_float(ac_mon, "f_max", s->ac_mon.f_max);
        float k = json_get_float(ac_mon, "cal_k", s->ac_mon.cal_k);
        s->ac_mon.cal_k = (k > 0.01f && k < 10.0f) ? k : 0.0f; /* sanity: anything else = Kconfig default */
    }
}

esp_err_t storage_load_config(app_config_t *cfg)
{
    /* app_config_t is ~26KB (MAX_PLUGS * sizeof(plug_t)) -- must not be a
     * stack local, it would blow the caller's task stack. */
    static app_config_t defaults;
    storage_default_config(&defaults);

    FILE *f = fopen(CONFIG_FILE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "No config.json yet, writing defaults");
        *cfg = defaults;
        return storage_save_config(cfg);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024) {
        fclose(f);
        ESP_LOGE(TAG, "config.json size invalid (%ld), using defaults", size);
        *cfg = defaults;
        return storage_save_config(cfg);
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "config.json parse error, using defaults");
        *cfg = defaults;
        return storage_save_config(cfg);
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->plug_count = 0;

    const cJSON *plugs = cJSON_GetObjectItemCaseSensitive(root, "plugs");
    if (cJSON_IsArray(plugs)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, plugs) {
            if (cfg->plug_count >= MAX_PLUGS) break;
            plug_from_json(item, &cfg->plugs[cfg->plug_count]);
            cfg->plug_count++;
        }
    }

    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (settings) {
        settings_from_json(settings, &cfg->settings, &defaults.settings);
    } else {
        cfg->settings = defaults.settings;
    }
    /* Belt-and-braces: guarantee GPIOs/poll interval match Kconfig
     * even if config.json was written by an older firmware that persisted
     * a different value. */
    storage_force_fixed_inputs(&cfg->settings);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded config: %d plug(s)", (int)cfg->plug_count);
    return ESP_OK;
}

esp_err_t storage_save_config(const app_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *plugs = cJSON_CreateArray();
    for (size_t i = 0; i < cfg->plug_count; i++) {
        plug_to_json(&cfg->plugs[i], plugs);
    }
    cJSON_AddItemToObject(root, "plugs", plugs);
    cJSON_AddItemToObject(root, "settings", settings_to_json(&cfg->settings));

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return ESP_ERR_NO_MEM;
    }

    const char *tmp_path = CONFIG_FILE_PATH ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        free(out);
        ESP_LOGE(TAG, "Failed to open %s for writing", tmp_path);
        return ESP_FAIL;
    }
    size_t len = strlen(out);
    size_t written = fwrite(out, 1, len, f);
    fclose(f);
    free(out);

    if (written != len) {
        ESP_LOGE(TAG, "Short write to config tmp file");
        remove(tmp_path);
        return ESP_FAIL;
    }

    remove(CONFIG_FILE_PATH);
    if (rename(tmp_path, CONFIG_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename tmp config into place");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved (%d bytes)", (int)len);
    return ESP_OK;
}

esp_err_t storage_erase_all(void)
{
    /* app_config_t is ~26KB (MAX_PLUGS * sizeof(plug_t)) -- same sizing
     * note as storage_load_config()'s `defaults` local and h_plugs_get() in
     * web_server.c. This is called from the web server's factory-reset
     * handler, whose httpd worker stack (12288 bytes, see
     * web_server_start()) is nowhere near big enough for a stack-local
     * copy, so heap-allocate it -- freed right after this one-shot,
     * user-initiated call, same as the export/import buffers above. */
    app_config_t *defaults = malloc(sizeof(app_config_t));
    if (!defaults) return ESP_ERR_NO_MEM;

    storage_default_config(defaults);
    esp_err_t err = storage_save_config(defaults);
    free(defaults);
    return err;
}
