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

void storage_default_config(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->plug_count = 0;

    cfg->settings.inputs[0] = (input_cfg_t){
        .id = INPUT_AC1, .label = "AC1",
        .gpio = CONFIG_INPUT_AC1_GPIO_DEFAULT, .enabled = true,
    };
    cfg->settings.inputs[1] = (input_cfg_t){
        .id = INPUT_DC1, .label = "DC1",
        .gpio = CONFIG_INPUT_DC1_GPIO_DEFAULT, .enabled = false,
    };
    cfg->settings.inputs[2] = (input_cfg_t){
        .id = INPUT_DC2, .label = "DC2",
        .gpio = CONFIG_INPUT_DC2_GPIO_DEFAULT, .enabled = false,
    };

    cfg->settings.debounce_ms = 50;
    cfg->settings.poll_interval_ms = 100;

    cfg->settings.mqtt_uri[0] = '\0';
    strncpy(cfg->settings.mqtt_client_id, "smart-plug-hub", sizeof(cfg->settings.mqtt_client_id) - 1);

    cfg->settings.log_max_file_size = 65536;
    cfg->settings.log_max_files = 5;
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
    cJSON_AddNumberToObject(o, "driver", (int)p->driver);

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

static void plug_from_json(const cJSON *o, plug_t *p)
{
    memset(p, 0, sizeof(*p));
    p->id = (uint32_t)json_get_int(o, "id", 0);
    json_get_str(o, "name", p->name, sizeof(p->name));
    p->enabled = json_get_bool(o, "enabled", true);
    p->input = (input_id_t)json_get_int(o, "input", INPUT_NONE);
    p->invert = json_get_bool(o, "invert", false);
    p->driver = (driver_type_t)json_get_int(o, "driver", DRIVER_HTTP);

    const cJSON *http = cJSON_GetObjectItemCaseSensitive(o, "http");
    if (http) {
        json_get_str(http, "on_url", p->http.on_url, sizeof(p->http.on_url));
        json_get_str(http, "off_url", p->http.off_url, sizeof(p->http.off_url));
        json_get_str(http, "status_url", p->http.status_url, sizeof(p->http.status_url));
        p->http.use_post = json_get_bool(http, "use_post", false);
        json_get_str(http, "auth_user", p->http.auth_user, sizeof(p->http.auth_user));
        json_get_str(http, "auth_pass", p->http.auth_pass, sizeof(p->http.auth_pass));
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
            s->inputs[i].gpio = json_get_int(item, "gpio", s->inputs[i].gpio);
            s->inputs[i].enabled = json_get_bool(item, "enabled", s->inputs[i].enabled);
            i++;
        }
    }

    s->debounce_ms = (uint32_t)json_get_int(root, "debounce_ms", (int)s->debounce_ms);
    s->poll_interval_ms = (uint32_t)json_get_int(root, "poll_interval_ms", (int)s->poll_interval_ms);
    json_get_str(root, "mqtt_uri", s->mqtt_uri, sizeof(s->mqtt_uri));
    json_get_str(root, "mqtt_user", s->mqtt_user, sizeof(s->mqtt_user));
    json_get_str(root, "mqtt_pass", s->mqtt_pass, sizeof(s->mqtt_pass));
    json_get_str(root, "mqtt_client_id", s->mqtt_client_id, sizeof(s->mqtt_client_id));
    s->log_max_file_size = (uint32_t)json_get_int(root, "log_max_file_size", (int)s->log_max_file_size);
    s->log_max_files = (uint32_t)json_get_int(root, "log_max_files", (int)s->log_max_files);
}

esp_err_t storage_load_config(app_config_t *cfg)
{
    app_config_t defaults;
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
