#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "plug_manager.h"
#include "storage.h"
#include "event_log.h"
#include "gpio_inputs.h"
#include "driver_http.h"
#include "driver_mqtt.h"
#include "driver_zigbee.h"
#include "wifi_manager.h"
#include "ac_monitor.h"

static const char *TAG = "plug_manager";

static app_config_t s_cfg;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_action_queue;
static uint32_t s_next_id = 1;
/* Latest in-band status from ac_monitor.c (see plug_manager_on_ac_mon_change()
 * below). Starts true -- matches ac_monitor.c's own "nothing to protect
 * against yet" default before its first real reading, so AC1-bound plugs
 * aren't held off at boot waiting for a sample window. Guarded by s_lock,
 * same as the rest of s_cfg. */
static bool s_ac_mon_in_band = true;

typedef struct {
    uint32_t plug_id;
    bool desired_on;
    char source[16];
} plug_action_t;

/* Caller must hold s_lock. */
static int find_index_locked(uint32_t id)
{
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        if (s_cfg.plugs[i].id == id) return (int)i;
    }
    return -1;
}

static esp_err_t save_locked(void)
{
    return storage_save_config(&s_cfg);
}

/* Runs the actual driver call for a plug snapshot (no locks held), then
 * updates runtime state under lock and writes an event log entry. */
static esp_err_t do_dispatch(plug_t snapshot, bool on, const char *source,
                              char *detail_out, size_t detail_out_size, bool *ok_out)
{
    /* 200, not 96: Cyrillic detail strings run ~2 bytes/character in UTF-8,
     * so a message that reads as ~100 characters on screen can be 180+
     * bytes on the wire -- a 96-byte buffer silently truncated those mid
     * multi-byte character, corrupting the UTF-8 the browser then tried to
     * decode. */
    char detail[200] = {0};
    esp_err_t err;

    /* See app_types.h's doc comment on plug_t.reachable: this flag is only
     * ever set false for the subset metrics_poll_task actively polls in the
     * background (HTTP + recognized family), which is exactly what
     * guarantees it un-sticks on its own once the device answers again --
     * so it's safe to hard-skip the network call here instead of blocking
     * whichever task called us (the single httpd worker, for a manual web
     * click; worker_task, for an input-triggered one) on a doomed multi-
     * second esp_http_client timeout. Without this, mashing a dead plug's
     * toggle a few times in a row queues up that many multi-second waits
     * back to back on the httpd's one worker task, stalling every other
     * request (including totally unrelated ones) until they all time out. */
    bool skip_unreachable = (snapshot.driver == DRIVER_HTTP &&
                              snapshot.http.family != PLUG_FAMILY_UNKNOWN &&
                              !snapshot.reachable);

    if (!snapshot.enabled) {
        snprintf(detail, sizeof(detail), "plug disabled");
        err = ESP_ERR_INVALID_STATE;
    } else if (skip_unreachable) {
        snprintf(detail, sizeof(detail), "plug not reachable (offline)");
        err = ESP_ERR_INVALID_STATE;
    } else {
        switch (snapshot.driver) {
            case DRIVER_HTTP:
                err = driver_http_set(&snapshot, on, detail, sizeof(detail));
                break;
            case DRIVER_MQTT:
                err = driver_mqtt_set(&snapshot, on, detail, sizeof(detail));
                break;
            case DRIVER_ZIGBEE:
                err = driver_zigbee_set(&snapshot, on, detail, sizeof(detail));
                break;
            default:
                snprintf(detail, sizeof(detail), "unknown driver type");
                err = ESP_ERR_INVALID_ARG;
                break;
        }
    }

    bool ok = (err == ESP_OK);

    /* Only overwrite ->reachable when this call actually attempted a
     * network round-trip to the device -- not when the plug is simply
     * disabled (unrelated to whether it's online), and not when
     * skip_unreachable already short-circuited above (nothing new was
     * learned, leave the existing false as-is rather than re-write it). */
    bool tracks_reachable = (snapshot.driver == DRIVER_HTTP && snapshot.http.family != PLUG_FAMILY_UNKNOWN);
    bool attempted_network_call = snapshot.enabled && !skip_unreachable;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(snapshot.id);
    if (idx >= 0) {
        s_cfg.plugs[idx].last_state = on;
        s_cfg.plugs[idx].last_cmd_ok = ok;
        s_cfg.plugs[idx].last_change_us = esp_timer_get_time();
        if (tracks_reachable && attempted_network_call) {
            s_cfg.plugs[idx].reachable = ok;
        }
    }
    xSemaphoreGive(s_lock);

    event_log_append(source, snapshot.name, on ? "on" : "off", ok, detail);

    if (detail_out) snprintf(detail_out, detail_out_size, "%s", detail);
    if (ok_out) *ok_out = ok;
    return err;
}

static void worker_task(void *arg)
{
    plug_action_t act;
    for (;;) {
        if (xQueueReceive(s_action_queue, &act, portMAX_DELAY) != pdTRUE) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        int idx = find_index_locked(act.plug_id);
        plug_t snapshot;
        bool have = false;
        if (idx >= 0) {
            snapshot = s_cfg.plugs[idx];
            have = true;
        }
        xSemaphoreGive(s_lock);

        if (have) {
            do_dispatch(snapshot, act.desired_on, act.source, NULL, 0, NULL);
        }
    }
}

/* Power-metering isn't time-critical (unlike on/off commands, which go
 * through the queue+worker_task path above and should feel instant) -- a
 * plain periodic poll loop is enough, and keeps it decoupled from the
 * action queue. Matches app.js's client-side auto-refresh interval (10s,
 * see loadPlugs()'s setInterval in app.js) -- there's no manual "Оновити"
 * button anymore, so the web UI only ever shows what this task last
 * fetched; keeping the two periods equal means a refresh in the browser
 * reliably picks up genuinely new numbers instead of the same stale
 * reading it already had (which the old 15s/10s mismatch could produce). */
#define METRICS_POLL_INTERVAL_MS 10000
#define METRICS_POLL_TASK_STACK  12288  /* esp_http_client-calling task, see worker_task's comment */

static void metrics_poll_task(void *arg)
{
    /* static, not a local array: MAX_PLUGS * sizeof(plug_t) is easily a few
     * KB, and this project has already hit one hard-to-diagnose stack
     * overflow from underestimating an esp_http_client-adjacent task's
     * stack needs (see README) -- not repeating that mistake by putting a
     * big buffer on top of it. */
    static plug_t snapshot[MAX_PLUGS];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(METRICS_POLL_INTERVAL_MS));

        /* Snapshot the plugs worth polling under lock, then do all the
         * (blocking, potentially slow) HTTP calls with no lock held --
         * same discipline do_dispatch()/plug_manager_add() use for the
         * on/off path. Only HTTP plugs with a fingerprint-recognized
         * family can possibly answer a metrics query (see
         * driver_http_get_metrics()). */
        size_t n = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < s_cfg.plug_count; i++) {
            plug_t *p = &s_cfg.plugs[i];
            if (p->enabled && p->driver == DRIVER_HTTP && p->http.family != PLUG_FAMILY_UNKNOWN) {
                snapshot[n++] = *p;
            }
        }
        xSemaphoreGive(s_lock);

        for (size_t i = 0; i < n; i++) {
            bool was_reachable = snapshot[i].reachable;

            plug_metrics_t m;
            bool metrics_ok = (driver_http_get_metrics(&snapshot[i], &m) == ESP_OK);

            /* Query actual on/off state every single cycle, not just metrics
             * -- a plug can change state without us ever sending a command
             * (its own physical button, another controller, a power cycle
             * defaulting to a particular relay state...), and metrics alone
             * (power/voltage/current) say nothing about on/off. last_state
             * is otherwise only ever touched by do_dispatch() (a command we
             * ourselves sent), so without this the ON/OFF pill could sit
             * showing stale state indefinitely even while the wattage
             * reading right next to it keeps updating normally every 10s --
             * confusing on its own (numbers changing, on/off pill frozen).
             * Same family restriction as driver_http_get_metrics(). For
             * Shelly Gen2 this happens to hit the exact same URL metrics
             * just used (both are /rpc/Switch.GetStatus) -- duplicating
             * that request instead of sharing the one response is wasteful,
             * but simple/uniform across all three families beats an early
             * optimization for what's still just one extra small LAN GET
             * per plug per 10s. */
            bool cur_on = false;
            bool status_ok = (driver_http_get_status(&snapshot[i], &cur_on) == ESP_OK);

            /* Reachable if EITHER query got through -- both talk to the
             * same device over the same kind of API, so either succeeding
             * is equally good evidence it's alive. */
            bool answered = metrics_ok || status_ok;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            int idx = find_index_locked(snapshot[i].id);
            if (idx >= 0) {
                /* This is the background signal do_dispatch() relies on to
                 * un-stick plug_t.reachable on its own once a plug comes back
                 * online (see its doc comment in app_types.h) -- update it
                 * every poll, success or failure, independent of the metrics
                 * reading itself. */
                s_cfg.plugs[idx].reachable = answered;
                if (metrics_ok) {
                    s_cfg.plugs[idx].metrics = m;
                    s_cfg.plugs[idx].metrics_updated_us = esp_timer_get_time();
                }
                /* else: network error / bad response -- leave the previous
                 * metrics reading (and its timestamp) as-is rather than
                 * blanking the UI over one dropped poll. */
                if (status_ok) {
                    /* Only bump last_change_us on a genuine transition (or
                     * recovering from a failed last_cmd_ok) -- otherwise
                     * every routine 10s re-check would stamp "just changed"
                     * on a relay that's been sitting in the same state for
                     * days. */
                    bool changed = (s_cfg.plugs[idx].last_state != cur_on) || !s_cfg.plugs[idx].last_cmd_ok;
                    s_cfg.plugs[idx].last_state = cur_on;
                    s_cfg.plugs[idx].last_cmd_ok = true;
                    if (changed) s_cfg.plugs[idx].last_change_us = esp_timer_get_time();
                }
            }
            xSemaphoreGive(s_lock);

            /* Worth a dedicated log line specifically for "came back after
             * being offline" (see plug_t.reachable's doc comment) -- routine
             * every-10s re-checks are deliberately NOT logged individually,
             * that would flood the event log with one entry per plug every
             * poll for no informational gain. */
            if (status_ok && !was_reachable) {
                event_log_append(LOG_SRC_SYSTEM, snapshot[i].name, cur_on ? "on" : "off", true,
                                  "reconnected, state re-checked");
            }
        }
    }
}

/* Boot-time "what's actually on right now" check for plugs restored from
 * config.json, PLUS pushing the current physical input state to every
 * input-bound plug -- both jobs share the exact same precondition (no point
 * doing either before the STA has an IP: reading a manual/HTTP plug's real
 * status needs a network call, and so does actually commanding an input-
 * bound plug's relay), so one wait-for-Wi-Fi dance covers both instead of
 * duplicating it.
 *
 * The input-sync half used to be a separate, unconditional call from
 * app_main.c (a flat 500ms delay after gpio_inputs_init(), long enough only
 * for GPIO debounce, then straight into plug_manager_sync_all_from_inputs()
 * regardless of whether Wi-Fi had come up yet). That's exactly what caused
 * "reboots with an inverted input don't actually apply it": the enqueued
 * command reached worker_task well before STA had an IP, driver_http_set()
 * spent its whole 5s timeout failing to even open a TCP connection
 * (ESP_ERR_HTTP_CONNECT), and -- critically -- nothing ever retried it, so
 * the relay just stayed wherever it happened to be at power-on, silently
 * disagreeing with the input the whole time.
 *
 * plug_manager_add() already does the "what's actually on" half for a
 * single plug at the moment it's created (see below); this is the same
 * idea applied once, at startup, to everything loaded from storage --
 * otherwise a saved plug always starts out reported as "off" until its
 * next command or metrics poll, even if the physical relay was left on
 * across a reboot. */
#define INITIAL_STATUS_TASK_STACK   12288 /* esp_http_client-calling task, see worker_task's comment */
#define INITIAL_STATUS_WAIT_STEP_MS 1000
#define INITIAL_STATUS_WAIT_MAX_MS  (30 * 1000)

static void initial_status_task(void *arg)
{
    (void)arg;

    /* This task starts well before STA association+DHCP can plausibly be
     * done (wifi_manager_init() only kicks off the connection attempt, it
     * doesn't block on it), so wait for a real connection first -- up to a
     * bounded timeout, and bailing out early if the STA config itself is
     * bad (wrong password etc., see wifi_manager_sta_config_error()) since
     * that's never going to resolve on its own. Skipping both halves of
     * this task entirely in either case is fine and correct, not just a
     * fallback: without any STA connectivity there is no network path to
     * command ANY HTTP or MQTT plug (input-bound or not) regardless of how
     * long we wait, so retrying the input sync separately wouldn't help --
     * plugs just keep their storage default (last_state = false) same as
     * before this feature existed, and the normal input-driven path
     * (plug_manager_on_input_change(), fired on the next real level change)
     * takes over once/if the network does come up. */
    int waited_ms = 0;
    while (!wifi_manager_is_connected() && waited_ms < INITIAL_STATUS_WAIT_MAX_MS) {
        if (wifi_manager_sta_config_error()) {
            ESP_LOGW(TAG, "initial status check: STA not configured/failed, skipping (incl. input sync)");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(INITIAL_STATUS_WAIT_STEP_MS));
        waited_ms += INITIAL_STATUS_WAIT_STEP_MS;
    }
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "initial status check: STA still not connected after %ds, skipping (incl. input sync)",
                 INITIAL_STATUS_WAIT_MAX_MS / 1000);
        vTaskDelete(NULL);
        return;
    }

    /* Same snapshot-under-lock / network-call-outside-lock / update-under-
     * lock discipline as metrics_poll_task() and plug_manager_add(). */
    static plug_t snapshot[MAX_PLUGS];
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        plug_t *p = &s_cfg.plugs[i];
        if (p->driver == DRIVER_HTTP && p->http.family != PLUG_FAMILY_UNKNOWN) {
            snapshot[n++] = *p;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "initial status check: querying %u saved plug(s)", (unsigned)n);
    size_t checked = 0;
    for (size_t i = 0; i < n; i++) {
        bool on = false;
        if (driver_http_get_status(&snapshot[i], &on) != ESP_OK) {
            ESP_LOGW(TAG, "initial status check: could not query \"%s\", leaving as-is", snapshot[i].name);
            continue;
        }
        checked++;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int idx = find_index_locked(snapshot[i].id);
        if (idx >= 0) {
            s_cfg.plugs[idx].last_state = on;
            s_cfg.plugs[idx].last_cmd_ok = true;
            s_cfg.plugs[idx].last_change_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
        event_log_append(LOG_SRC_BOOT, snapshot[i].name, on ? "on" : "off", true, "initial status check");
    }
    ESP_LOGI(TAG, "initial status check: %u/%u plug(s) confirmed", (unsigned)checked, (unsigned)n);

    /* Now that STA is confirmed connected, push the current physical input
     * state to every input-bound plug (see this function's top comment for
     * why this has to wait for the same thing the status check above does).
     * Unlike the loop above, this isn't restricted to fingerprinted HTTP
     * families -- plug_manager_sync_all_from_inputs() covers any driver
     * (HTTP with a manual/unrecognized URL, MQTT too), same as it already
     * does when called after an inputs-tab settings save. */
    plug_manager_sync_all_from_inputs();

    vTaskDelete(NULL);
}

static void enqueue_action(uint32_t plug_id, bool on, const char *source)
{
    plug_action_t act = { .plug_id = plug_id, .desired_on = on };
    strncpy(act.source, source, sizeof(act.source) - 1);
    if (xQueueSend(s_action_queue, &act, 0) != pdTRUE) {
        ESP_LOGW(TAG, "action queue full, dropping command for plug %u", (unsigned)plug_id);
    }
}

esp_err_t plug_manager_init(const app_config_t *initial_cfg)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_cfg = *initial_cfg;
    storage_force_fixed_inputs(&s_cfg.settings);

    uint32_t max_id = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        if (s_cfg.plugs[i].id > max_id) max_id = s_cfg.plugs[i].id;
        s_cfg.plugs[i].last_state = false;
        s_cfg.plugs[i].last_cmd_ok = true;
        s_cfg.plugs[i].last_change_us = 0;
        memset(&s_cfg.plugs[i].metrics, 0, sizeof(s_cfg.plugs[i].metrics));
        s_cfg.plugs[i].metrics_updated_us = 0;
        s_cfg.plugs[i].reachable = true; /* optimistic until the first poll/command says otherwise */
    }
    s_next_id = max_id + 1;

    s_action_queue = xQueueCreate(32, sizeof(plug_action_t));
    if (!s_action_queue) return ESP_ERR_NO_MEM;

    /* esp_http_client (DNS resolution, TCP/TLS transport, header parsing)
     * needs considerably more than 4KB of stack -- 4096 overflowed as soon
     * as a plug with an HTTP driver actually got dispatched. 8192 was
     * enough on classic ESP32 (Xtensa LX6) but produced a Guru Meditation
     * LoadProhibited panic *inside the FreeRTOS scheduler itself* (poisoned
     * 0xa5a5a5a5 registers -- the canary-based overflow check only
     * verifies the last few bytes of the stack on each context switch, so
     * a single deep call chain that overshoots it entirely goes
     * undetected until something else touches the corrupted memory) the
     * moment a real esp_http_client call ran on ESP32-S3 (LX7) -- deeper
     * call chains there apparently use more stack. Bumped with real
     * headroom instead of nudging it up by a few hundred bytes again. */
    BaseType_t ok = xTaskCreate(worker_task, "plug_worker", 12288, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    /* Lower priority than worker_task -- background metering polls should
     * never delay an actual on/off command. */
    BaseType_t okm = xTaskCreate(metrics_poll_task, "plug_metrics", METRICS_POLL_TASK_STACK, NULL, 3, NULL);
    if (okm != pdPASS) return ESP_ERR_NO_MEM;

    /* One-shot, not periodic -- deletes itself once done. Not treated as a
     * fatal init failure if it can't be started: it's a nice-to-have, and
     * plugs already have a well-defined (if occasionally stale) fallback
     * of "assume off". */
    BaseType_t oki = xTaskCreate(initial_status_task, "plug_init_status", INITIAL_STATUS_TASK_STACK, NULL, 3, NULL);
    if (oki != pdPASS) {
        ESP_LOGW(TAG, "could not start initial status check task, saved plugs keep default state");
    }

    driver_mqtt_apply_settings(&s_cfg.settings);

    return ESP_OK;
}

/* AC1-bound plugs get an extra safety gate on top of the normal
 * level^invert logic: if AC mains monitoring is enabled and the last AC
 * monitor reading was out of the configured voltage/frequency band, force
 * the plug off regardless of what the AC1 digital input itself says --
 * out-of-band mains is exactly the situation an AC1-triggered plug (e.g.
 * "turn on when mains is present") should NOT be energized in. Caller must
 * already hold s_lock. */
static bool gate_ac1(const plug_t *p, bool want)
{
    if (p->input != INPUT_AC1) return want;
    if (!s_cfg.settings.ac_mon.enabled) return want;
    if (!s_ac_mon_in_band) return false;
    return want;
}

void plug_manager_on_input_change(input_id_t id, bool level)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t targets[MAX_PLUGS];
    bool desired[MAX_PLUGS];
    size_t n = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        plug_t *p = &s_cfg.plugs[i];
        if (!p->enabled || p->input != id) continue;
        targets[n] = p->id;
        desired[n] = gate_ac1(p, level ^ p->invert);
        n++;
    }
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < n; i++) {
        enqueue_action(targets[i], desired[i], LOG_SRC_INPUT);
    }
}

void plug_manager_sync_all_from_inputs(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t targets[MAX_PLUGS];
    bool desired[MAX_PLUGS];
    size_t n = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        plug_t *p = &s_cfg.plugs[i];
        if (!p->enabled || p->input == INPUT_NONE) continue;
        if (!gpio_inputs_is_enabled(p->input)) continue;
        bool level = gpio_inputs_get_level(p->input);
        targets[n] = p->id;
        desired[n] = gate_ac1(p, level ^ p->invert);
        n++;
    }
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < n; i++) {
        enqueue_action(targets[i], desired[i], LOG_SRC_BOOT);
    }
}

/* ac_monitor.c callback target -- fired only on an actual in-band/out-of-band
 * transition (see ac_monitor.h). Re-evaluates every AC1-bound plug against
 * the new status: going out of band force-drops them (see gate_ac1() above),
 * coming back in band re-applies whatever the AC1 input itself currently
 * says. Plugs bound to other inputs, or not bound to any input, are
 * untouched -- this is deliberately scoped to AC1 only, same as
 * plug_manager_on_input_change()'s own per-input filtering. */
void plug_manager_on_ac_mon_change(bool in_band)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ac_mon_in_band = in_band;
    uint32_t targets[MAX_PLUGS];
    bool desired[MAX_PLUGS];
    size_t n = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        plug_t *p = &s_cfg.plugs[i];
        if (!p->enabled || p->input != INPUT_AC1) continue;
        if (!gpio_inputs_is_enabled(p->input)) continue;
        bool level = gpio_inputs_get_level(p->input);
        targets[n] = p->id;
        desired[n] = gate_ac1(p, level ^ p->invert);
        n++;
    }
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < n; i++) {
        enqueue_action(targets[i], desired[i], LOG_SRC_AC_MON);
    }
}

size_t plug_manager_get_all(plug_t *out, size_t max_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_cfg.plug_count < max_out ? s_cfg.plug_count : max_out;
    memcpy(out, s_cfg.plugs, n * sizeof(plug_t));
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t plug_manager_get(uint32_t id, plug_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    if (idx >= 0) *out = s_cfg.plugs[idx];
    xSemaphoreGive(s_lock);
    return idx >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t plug_manager_add(plug_t *in_out)
{
    /* Query the device's real current on/off state before touching the
     * lock -- this is a network call (up to a few seconds), and it must
     * never run while s_lock is held, same discipline do_dispatch() uses
     * for the normal on/off path. Only possible for HTTP plugs with a
     * fingerprint-recognized family (Shelly/Tasmota); anything else keeps
     * the previous "assume off, not a reported failure" default -- there's
     * no reliable way to ask an unrecognized/manual endpoint for its state. */
    bool initial_on = false;
    bool got_status = false;
    if (in_out->driver == DRIVER_HTTP && in_out->http.family != PLUG_FAMILY_UNKNOWN) {
        got_status = (driver_http_get_status(in_out, &initial_on) == ESP_OK);
        if (!got_status) {
            ESP_LOGW(TAG, "could not query initial status for \"%s\", defaulting to off", in_out->name);
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.plug_count >= MAX_PLUGS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    in_out->id = s_next_id++;
    in_out->last_state = got_status ? initial_on : false;
    in_out->last_cmd_ok = true; /* either genuinely confirmed, or "no info yet", not a failure */
    in_out->last_change_us = esp_timer_get_time();
    /* Reflect the query attempt just above for tracked plugs (see
     * app_types.h's doc comment on ->reachable) -- an untracked plug
     * (manual/unrecognized-family HTTP, MQTT, Zigbee) always starts
     * optimistic since nothing will ever probe it in the background. */
    in_out->reachable = (in_out->driver == DRIVER_HTTP && in_out->http.family != PLUG_FAMILY_UNKNOWN)
                             ? got_status
                             : true;
    s_cfg.plugs[s_cfg.plug_count] = *in_out;
    s_cfg.plug_count++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    event_log_append(LOG_SRC_API, in_out->name, "created", true, NULL);
    return err;
}

esp_err_t plug_manager_update(uint32_t id, const plug_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    plug_t updated = *in;
    updated.id = id;
    updated.last_state = s_cfg.plugs[idx].last_state;
    updated.last_cmd_ok = s_cfg.plugs[idx].last_cmd_ok;
    updated.last_change_us = s_cfg.plugs[idx].last_change_us;
    updated.metrics = s_cfg.plugs[idx].metrics;
    updated.metrics_updated_us = s_cfg.plugs[idx].metrics_updated_us;
    updated.reachable = s_cfg.plugs[idx].reachable;
    s_cfg.plugs[idx] = updated;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    event_log_append(LOG_SRC_API, updated.name, "updated", true, NULL);

    /* Editing a plug's "Вхід"/"Інверсія" only takes effect the *next* time
     * that physical input actually transitions (plug_manager_on_input_change
     * only runs on a change, see gpio_inputs.c's poll_task) -- without this,
     * saving a new invert setting while the input has been sitting at a
     * steady level for a while (the common case -- inputs don't flip just
     * because someone edited a plug) would silently do nothing until the
     * input happened to toggle again, which looks exactly like "the
     * checkbox doesn't work". Re-evaluate every enabled, input-bound plug
     * against the input's *current* level right now instead of waiting.
     * Same call plug_manager_update_settings() already makes after an
     * inputs-tab save, for the identical reason. */
    plug_manager_sync_all_from_inputs();
    return err;
}

esp_err_t plug_manager_delete(uint32_t id)
{
    char name[PLUG_NAME_LEN] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    strncpy(name, s_cfg.plugs[idx].name, sizeof(name) - 1);
    for (size_t i = idx; i + 1 < s_cfg.plug_count; i++) {
        s_cfg.plugs[i] = s_cfg.plugs[i + 1];
    }
    s_cfg.plug_count--;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    event_log_append(LOG_SRC_API, name, "deleted", true, NULL);
    return err;
}

esp_err_t plug_manager_test_action(uint32_t id, bool on, char *detail, size_t detail_size, bool *ok_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    plug_t snapshot;
    if (idx >= 0) snapshot = s_cfg.plugs[idx];
    xSemaphoreGive(s_lock);

    if (idx < 0) {
        if (detail) snprintf(detail, detail_size, "plug not found");
        if (ok_out) *ok_out = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* A plug bound to a physical input is driven exclusively by that input
     * (see plug_manager_on_input_change()/plug_manager_sync_all_from_
     * inputs()) -- letting a manual command through here as well means two
     * competing sources of truth for the same relay: the input can flip it
     * back the moment it next changes (or right now, via the sync call
     * above), silently undoing whatever the manual click just did. Refusing
     * it here (rather than just hiding the button in the web UI, which
     * plug_manager_test_action() itself doesn't know or enforce) also
     * protects the /api/plugs/{id}/action endpoint directly, not just the
     * one caller that happens to grey out its own button. */
    if (snapshot.input != INPUT_NONE) {
        if (detail) snprintf(detail, detail_size, "керується входом. Для ручного керування виберіть відповідне налаштування.");
        if (ok_out) *ok_out = false;
        return ESP_ERR_INVALID_STATE;
    }

    return do_dispatch(snapshot, on, LOG_SRC_MANUAL, detail, detail_size, ok_out);
}

esp_err_t plug_manager_get_settings(system_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg.settings;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t plug_manager_update_settings(const system_settings_t *in)
{
    system_settings_t new_settings = *in;
    storage_force_fixed_inputs(&new_settings); /* gpio is never web-editable */

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.settings = new_settings;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    gpio_inputs_update_settings(&new_settings);
    event_log_update_limits(new_settings.log_max_file_size, new_settings.log_max_files);
    driver_mqtt_apply_settings(&new_settings);
    ac_monitor_update_settings(&new_settings.ac_mon);

    event_log_append(LOG_SRC_SYSTEM, "-", "settings_updated", true, NULL);

    plug_manager_sync_all_from_inputs();
    return err;
}

bool plug_manager_is_responsive(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    xSemaphoreGive(s_lock);
    return true;
}
