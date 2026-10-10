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

/* Per-plug turn-ON delay limit, seconds (plug_t.on_delay_s). */
#define PLUG_ON_DELAY_S_MAX  600

#define STORAGE_MOUNT_POINT  "/storage"
#define CONFIG_FILE_PATH     STORAGE_MOUNT_POINT "/config.json"
#define LOG_FILE_PATH        STORAGE_MOUNT_POINT "/log.txt"

/* Logical trigger inputs. Fixed set of 3 slots, purpose given by the user's
 * wiring. DC1/DC2 are real physical GPIO lines, debounced and polled by
 * gpio_inputs.c. AC1 is NOT a physical GPIO on this board revision -- its
 * "level" is derived entirely from the AC mains monitor's in-band status
 * (see ac_monitor.c / plug_manager.c's plug_manager_on_ac_mon_change()):
 * active exactly when the last ZMPT101B reading's voltage AND frequency are
 * within the configured system_settings_t.ac_mon band. gpio_inputs.c still
 * carries an (unused, -1) slot for it so the fixed 3-entry inputs[] array,
 * storage/web-API shape and the AC1/DC1/DC2 labels stay uniform. NONE means
 * "not bound to any input" (plug is manual/API-only). */
typedef enum {
    INPUT_NONE = 0,
    INPUT_AC1  = 1, /* virtual -- driven by ac_monitor.c, no physical GPIO */
    INPUT_DC1  = 2,
    INPUT_DC2  = 3,
    INPUT_COUNT_PHYSICAL = 3, /* AC1 (virtual), DC1, DC2 -- array sizing only, not "3 real GPIOs" */
} input_id_t;

typedef enum {
    DRIVER_HTTP   = 0,
    DRIVER_MQTT   = 1,
    DRIVER_ZIGBEE = 2, /* reserved / not yet implemented */
} driver_type_t;

/* Result of fingerprint.c's protocol cascade against a discovered HTTP host.
 * "Gen2" covers Shelly Gen2, Gen3 and newer -- their RPC shape is identical,
 * see fingerprint.c for details. Control still goes through the plain
 * DRIVER_HTTP path (driver_http.c) -- this only decides which on_url/off_url
 * template to prefill, not a separate runtime driver. */
typedef enum {
    PLUG_FAMILY_UNKNOWN     = 0,
    PLUG_FAMILY_SHELLY_GEN1 = 1,
    PLUG_FAMILY_SHELLY_GEN2 = 2,
    PLUG_FAMILY_TASMOTA     = 3,
    PLUG_FAMILY_ESPHOME     = 4,
} plug_family_t;

/* What kind of accessory this looks like, so a relay/light found during a
 * scan doesn't get mistaken for a switchable plug. */
typedef enum {
    PLUG_KIND_UNKNOWN = 0,
    PLUG_KIND_PLUG     = 1,
    PLUG_KIND_RELAY    = 2,
    PLUG_KIND_LIGHT    = 3,
    PLUG_KIND_OTHER    = 4,
} plug_kind_t;

typedef struct {
    char on_url[URL_LEN];
    char off_url[URL_LEN];
    char status_url[URL_LEN]; /* optional, empty = not used */
    bool use_post;            /* false = GET, true = POST */
    char auth_user[AUTH_LEN]; /* optional basic auth, empty = none */
    char auth_pass[AUTH_LEN];

    /* Set (and persisted) when this plug was added by picking a
     * fingerprint.c-recognized result from the network scan. UNKNOWN for
     * anything entered by hand. When known, the web UI hides the on_url/
     * off_url/use_post fields instead of offering them for editing (the
     * whole point of fingerprinting was to not make the user hunt down
     * their plug's API), and plug_manager_add() uses it to query the
     * device's real on/off state via driver_http_get_status() instead of
     * defaulting a freshly-added plug to "off". */
    plug_family_t family;
    plug_kind_t kind;
} http_driver_cfg_t;

/* Best-effort power-metering reading, queried periodically (see
 * plug_manager.c's metrics_poll_task) via driver_http_get_metrics() for
 * HTTP plugs with a recognized, metering-capable family. "valid" is false
 * (with the numeric fields left at 0) whenever the device answered but
 * doesn't actually report power data (e.g. a non-PM Shelly relay) -- that's
 * different from a network/query failure, which just leaves the previous
 * reading (and updated_us) untouched so the UI doesn't flicker to blank on
 * one dropped poll. voltage_v/current_a may be 0 even when valid is true --
 * Shelly Gen1 Plug/PlugS only ever reports power, no voltage/current. */
typedef struct {
    bool valid;
    float power_w;
    float voltage_v;
    float current_a;
} plug_metrics_t;

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

    /* Turn-ON delay (seconds, 0..PLUG_ON_DELAY_S_MAX) before actually switching this plug ON --
     * removes relay-contact bounce on power-up. OFF is always dispatched
     * immediately, never delayed; only ON ever waits. 0 = no delay (ON
     * applies immediately, same behavior as before this setting existed).
     * Applied by plug_manager.c to the *already inverted* on/off decision:
     * callers compute `level ^ invert` first and hand plug_manager the
     * resulting desired state, so this delay automatically follows
     * whichever physical edge `invert` maps to "on" -- no separate
     * inversion handling is needed here. */
    uint32_t on_delay_s;
    driver_type_t driver;
    http_driver_cfg_t  http;
    mqtt_driver_cfg_t  mqtt;
    zigbee_driver_cfg_t zigbee;

    /* runtime state, not persisted */
    bool last_state;
    bool last_cmd_ok;
    int64_t last_change_us;
    plug_metrics_t metrics;         /* last successful power-metering reading */
    int64_t metrics_updated_us;     /* 0 = never successfully polled */

    /* Best-effort "is this device currently answering" flag, kept up to date
     * by plug_manager.c's metrics_poll_task (every METRICS_POLL_INTERVAL_MS)
     * and by do_dispatch() after every actual on/off attempt -- but ONLY for
     * DRIVER_HTTP plugs with a fingerprint-recognized family, i.e. the exact
     * same subset metrics_poll_task already polls in the background (see its
     * comment in plug_manager.c). That background poll is what guarantees
     * this flag eventually flips back to true once the device comes back,
     * even if nobody happens to click its toggle again -- a manual/
     * unrecognized-family HTTP plug has no such background poll, so it's
     * deliberately left out of this tracking and always stays true (nothing
     * would ever clear a false back to true for it). Defaults to true
     * (optimistic: "no reason yet to think it's offline"), same philosophy
     * as last_cmd_ok. Used to grey out the web UI's toggle button and skip
     * do_dispatch()'s network call (and its multi-second timeout) instead of
     * piling up doomed requests when the device is known to be offline. */
    bool reachable;

    /* Anti-chatter ON-delay bookkeeping (see on_delay_s above), guarded by
     * plug_manager.c's s_lock same as the rest of this struct -- not
     * persisted, reset to "nothing pending" on every boot.
     * on_delay_due_us: 0 means no delayed ON is currently pending for this
     * plug; otherwise it's the esp_timer_get_time() deadline at which
     * plug_manager.c's on_delay_task() should actually dispatch the ON.
     * Cleared back to 0 either when that dispatch happens, or immediately
     * if a newer command supersedes it first (an OFF always cancels a
     * pending ON outright; another ON just reschedules the deadline).
     * on_delay_source: the log source string (see event_log.h's LOG_SRC_*)
     * to attribute the eventual delayed dispatch to, captured at schedule
     * time since the original caller's stack frame is long gone by the
     * time on_delay_task() fires. */
    int64_t on_delay_due_us;
    char on_delay_source[16];
} plug_t;

typedef struct {
    input_id_t id;
    char label[8];    /* "AC1", "DC1", "DC2" */
    int  gpio;        /* fixed by Kconfig (see storage_force_fixed_inputs() in
                        * storage.c) -- never editable via config.json or the
                        * web UI, only "enabled" is. -1 for AC1: it has no
                        * physical GPIO, see input_id_t's comment above. */
    bool enabled;      /* system setting: is this input actively listened to */
} input_cfg_t;

/* AC mains quality monitor (ac_monitor.c): a ZMPT101B module wired to a
 * dedicated ADC-capable GPIO (not one of the inputs[] lines above -- this
 * reads an analog waveform, not a digital level) provides RMS voltage and
 * frequency. min/max here are the "normal" band the user expects their
 * mains to stay within; ac_monitor.c compares each new reading against
 * them and exposes the result (ac_mon_reading_t.in_band) for whoever wants
 * to act on it. Voltage is whole volts (1V steps are plenty for a mains
 * quality check); frequency keeps two decimal places since mains frequency
 * normally only drifts by hundredths of a Hz even under real grid stress.
 *
 * No on/off flag here on purpose -- AC1's entire reason to exist is
 * monitoring the mains (see input_id_t's comment above on AC1), so
 * an "enable monitoring" toggle would have nothing to turn off without
 * also disabling AC1 itself. Band checking is unconditionally active
 * whenever ac_monitor.c has a real reading. */
typedef struct {
    int   v_min;    /* volts */
    int   v_max;    /* volts */
    float f_min;    /* Hz, 0.01 precision */
    float f_max;    /* Hz, 0.01 precision */
    float cal_k;    /* voltmeter calibration, volts of mains per mV RMS at the ADC pin. Set only via
                     * POST /api/ac/calibrate (never from /api/settings); 0 = use the Kconfig default */
} ac_mon_settings_t;

typedef struct {
    input_cfg_t inputs[INPUT_COUNT_PHYSICAL];
    uint32_t poll_interval_ms;
    ac_mon_settings_t ac_mon;

    char mqtt_uri[96];         /* e.g. mqtt://user:pass@host:1883 or empty to disable */
    char mqtt_user[AUTH_LEN];
    char mqtt_pass[AUTH_LEN];
    char mqtt_client_id[32];

    /* No more log_max_file_size/log_max_files here -- the event log is a
     * fixed EVENT_LOG_MAX_LINES (50) most-recent entries now, not a
     * user-tunable byte-size/rotation-count pair (see event_log.c). */

    /* NTP server is intentionally NOT here -- it's fixed at build time via
     * CONFIG_NTP_SERVER_DEFAULT (Kconfig), not runtime/web-editable. See
     * time_sync_init() call in app_main.c. Manual clock set (independent of
     * NTP) is still available via POST /api/time -> time_sync_set_manual(). */
} system_settings_t;

typedef struct {
    plug_t plugs[MAX_PLUGS];
    size_t plug_count;
    system_settings_t settings;
} app_config_t;

#ifdef __cplusplus
}
#endif
