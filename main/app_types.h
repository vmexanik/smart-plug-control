#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PLUGS           32
#define PLUG_NAME_LEN        32
#define URL_LEN             128
#define TOPIC_LEN            96
#define PAYLOAD_LEN          32
#define AUTH_LEN             32

#define STORAGE_MOUNT_POINT  "/storage"
#define CONFIG_FILE_PATH     STORAGE_MOUNT_POINT "/config.json"
#define LOG_FILE_PATH        STORAGE_MOUNT_POINT "/log.txt"

/* Logical trigger inputs. Fixed set of 3 physical lines, purpose given by the
 * user's wiring (AC opto input, two DC inputs). NONE means "not bound to a
 * physical line" (plug is manual/API-only). */
typedef enum {
    INPUT_NONE = 0,
    INPUT_AC1  = 1,
    INPUT_DC1  = 2,
    INPUT_DC2  = 3,
    INPUT_COUNT_PHYSICAL = 3, /* AC1, DC1, DC2 */
} input_id_t;

typedef enum {
    DRIVER_HTTP   = 0,
    DRIVER_MQTT   = 1,
    DRIVER_ZIGBEE = 2, /* reserved / not yet implemented */
} driver_type_t;

typedef struct {
    char on_url[URL_LEN];
    char off_url[URL_LEN];
    char status_url[URL_LEN]; /* optional, empty = not used */
    bool use_post;            /* false = GET, true = POST */
    char auth_user[AUTH_LEN]; /* optional basic auth, empty = none */
    char auth_pass[AUTH_LEN];
} http_driver_cfg_t;

typedef struct {
    char cmd_topic[TOPIC_LEN];
    char payload_on[PAYLOAD_LEN];
    char payload_off[PAYLOAD_LEN];
    char state_topic[TOPIC_LEN]; /* optional, empty = not used */
    int  qos;
    bool retain;
} mqtt_driver_cfg_t;

typedef struct {
    /* Placeholder for future esp-zigbee-sdk integration
     * (endpoint id, cluster, short/ieee address, ...). */
    char note[64];
} zigbee_driver_cfg_t;

typedef struct {
    uint32_t id;
    char name[PLUG_NAME_LEN];
    bool enabled;               /* master enable, disabled plugs are ignored by input logic */
    input_id_t input;           /* which physical input drives this plug, INPUT_NONE = manual only */
    bool invert;                /* invert the input level before applying it */
    driver_type_t driver;
    http_driver_cfg_t  http;
    mqtt_driver_cfg_t  mqtt;
    zigbee_driver_cfg_t zigbee;

    /* runtime state, not persisted */
    bool last_state;
    bool last_cmd_ok;
    int64_t last_change_us;
} plug_t;

typedef struct {
    input_id_t id;
    char label[8];    /* "AC1", "DC1", "DC2" */
    int  gpio;
    bool enabled;      /* system setting: is this input actively listened to */
} input_cfg_t;

typedef struct {
    input_cfg_t inputs[INPUT_COUNT_PHYSICAL];
    uint32_t debounce_ms;
    uint32_t poll_interval_ms;

    char mqtt_uri[96];         /* e.g. mqtt://user:pass@host:1883 or empty to disable */
    char mqtt_user[AUTH_LEN];
    char mqtt_pass[AUTH_LEN];
    char mqtt_client_id[32];

    uint32_t log_max_file_size;
    uint32_t log_max_files;
} system_settings_t;

typedef struct {
    plug_t plugs[MAX_PLUGS];
    size_t plug_count;
    system_settings_t settings;
} app_config_t;

#ifdef __cplusplus
}
#endif
