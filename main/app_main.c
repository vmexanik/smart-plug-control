#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_types.h"
#include "storage.h"
#include "event_log.h"
#include "wifi_manager.h"
#include "gpio_inputs.h"
#include "plug_manager.h"
#include "web_server.h"
#include "time_sync.h"
#include "net_scan.h"
#include "system_health.h"
#include "led_status.h"
#include "captive_dns.h"
#include "ac_monitor.h"

static const char *TAG = "app_main";

/* Human-readable label for esp_reset_reason() -- only the ones worth calling
 * out specially are named; anything else (deep sleep wake causes, SDIO,
 * USB, etc. -- none of which apply to this device) falls back to a numeric
 * label rather than silently mapping to "unknown". */
static const char *reset_reason_str(esp_reset_reason_t r, bool *out_abnormal)
{
    *out_abnormal = false;
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software (esp_restart)";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_EXT:      return "external pin/reset button";
        case ESP_RST_BROWNOUT: *out_abnormal = true; return "brownout (power supply dip)";
        case ESP_RST_PANIC:    *out_abnormal = true; return "firmware panic (crash)";
        case ESP_RST_INT_WDT:  *out_abnormal = true; return "interrupt watchdog (CPU core hung/hogged)";
        case ESP_RST_TASK_WDT: *out_abnormal = true; return "task watchdog (a task/init step didn't respond in time)";
        case ESP_RST_WDT:      *out_abnormal = true; return "other watchdog";
        default:                return "other/unknown";
    }
}

/* Boot-time watchdog coverage.
 *
 * Until this commit, only system_health.c's own background task was
 * subscribed to the task watchdog (TWDT) -- nothing watched app_main()
 * itself while it ran through nvs/storage/Wi-Fi/plug_manager/gpio/net_scan/
 * httpd init, all sequentially, all before system_health_init() even runs.
 * A hang in any one of those (this is exactly what happened after the power
 * cut that prompted this: the device never came back up, and there was
 * nothing in the log because whatever hung did so before -- or during --
 * event_log_init() itself) had no way to resolve on its own: no reset, no
 * retry, nothing. The CPU wasn't "busy" in the sense the default idle-task
 * watchdog checks for either (a blocked wait on a semaphore/mutex that never
 * gets signaled yields the CPU just fine, so idle tasks keep running and
 * that alone never trips) -- it would just sit there forever.
 *
 * Subscribing this task (the one running app_main()) to the TWDT and
 * resetting it after each step means: if any single step takes longer than
 * CONFIG_ESP_TASK_WDT_TIMEOUT_S (5s, sdkconfig) to return, the watchdog
 * fires -- CONFIG_ESP_TASK_WDT_PANIC=y (already enabled, see
 * sdkconfig.defaults) turns that into an actual reset rather than just a
 * logged warning. The *next* boot then logs the previous reset reason
 * below, so even though the hung boot itself couldn't write anything, the
 * recovery boot afterward leaves a trail explaining what happened. Must be
 * unsubscribed (esp_task_wdt_delete()) right before app_main() returns --
 * this task is deleted by the IDF startup code at that point, so nothing
 * would ever call esp_task_wdt_reset() for it again, and a stale
 * subscription would just fire a bogus reset a few seconds into normal
 * operation. */
#define BOOT_WDT_STEP() esp_task_wdt_reset()

void app_main(void)
{
    /* Blinking from the very first possible moment, independent of
     * everything else -- see led_status.h. If a later step below hangs
     * (and the boot watchdog hasn't fired yet, or TWDT itself is somehow
     * unavailable), a still-blinking LED at least tells you the CPU itself
     * is alive and it's a specific subsystem that's stuck, not a total
     * freeze. Not treated as fatal if it fails to start (a spare GPIO
     * blinking is not worth refusing to boot over). */
    if (led_status_init() != ESP_OK) {
        ESP_LOGW(TAG, "could not start status LED task");
    }

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "could not subscribe boot sequence to task watchdog: %s", esp_err_to_name(wdt_err));
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    BOOT_WDT_STEP();

    ESP_ERROR_CHECK(storage_init());
    BOOT_WDT_STEP();

    /* app_config_t is ~26KB (MAX_PLUGS * sizeof(plug_t)) -- far too big for
     * a stack local given the main task's stack size, so it lives in .bss. */
    static app_config_t cfg;
    ESP_ERROR_CHECK(storage_load_config(&cfg));
    BOOT_WDT_STEP();

    ESP_ERROR_CHECK(event_log_init(&cfg.settings));
    event_log_append(LOG_SRC_BOOT, "-", "boot", true, "firmware started");

    /* Now that logging actually works, record *why* this boot happened --
     * the one case this can never catch is the very hang that prompted this
     * whole mechanism (nothing can log a hang that's still in progress),
     * but every reset the watchdog causes from now on leaves a note here on
     * the next successful boot. */
    bool reset_abnormal = false;
    const char *reset_str = reset_reason_str(esp_reset_reason(), &reset_abnormal);
    event_log_append(LOG_SRC_BOOT, "-", "reset_reason", !reset_abnormal, reset_str);
    if (reset_abnormal) {
        ESP_LOGW(TAG, "previous reset was abnormal: %s", reset_str);
    }
    BOOT_WDT_STEP();

    ESP_ERROR_CHECK(wifi_manager_init());
    BOOT_WDT_STEP();

    /* Captive-portal DNS hijack for the fallback AP -- see captive_dns.h.
     * Not treated as fatal if it fails to start: the AP and its web UI are
     * still fully usable by IP even without the auto-popup convenience. */
    if (captive_dns_init() != ESP_OK) {
        ESP_LOGW(TAG, "could not start captive-portal DNS server");
    }
    BOOT_WDT_STEP();

    /* NTP server is fixed at build time (Kconfig), not web-editable. This
     * call is non-blocking: arms the SNTP client, actual sync happens once
     * STA has an IP (or never, if AP-only -- the web UI's manual time set
     * via POST /api/time covers that case). */
    time_sync_init(CONFIG_NTP_SERVER_DEFAULT);

    ESP_ERROR_CHECK(plug_manager_init(&cfg));
    BOOT_WDT_STEP();

    ESP_ERROR_CHECK(gpio_inputs_init(&cfg.settings, plug_manager_on_input_change));
    BOOT_WDT_STEP();

    /* AC mains voltage/frequency monitor (ZMPT101B on GPIO7/ADC1_CH6) --
     * not treated as fatal if it fails to start: this is an optional
     * sensor, most of the firmware works fine without it. */
    if (ac_monitor_init(&cfg.settings.ac_mon, plug_manager_on_ac_mon_change) != ESP_OK) {
        ESP_LOGW(TAG, "could not start AC mains monitor");
    }
    BOOT_WDT_STEP();

    ESP_ERROR_CHECK(net_scan_init());
    BOOT_WDT_STEP();

    ESP_ERROR_CHECK(web_server_start());
    BOOT_WDT_STEP();

    /* Background reliability watchdog -- deadlock/Wi-Fi-stuck detection and
     * a periodic forensic heartbeat, see system_health.h for the full
     * rationale. Not treated as fatal if it fails to start (unlikely --
     * it's a tiny task): losing this safety net shouldn't take down a
     * device that's otherwise working. */
    if (system_health_init() != ESP_OK) {
        ESP_LOGW(TAG, "could not start system_health task, running without the reliability watchdog");
    }
    BOOT_WDT_STEP();

    /* Pushing the current physical input state to bound plugs used to
     * happen right here (a flat 500ms delay, then
     * plug_manager_sync_all_from_inputs()) -- moved into plug_manager.c's
     * initial_status_task() instead, because 500ms is nowhere near enough
     * time for STA to have an IP yet, and dispatching an actual on/off
     * command before the network is up just burns driver_http_set()'s full
     * 5s timeout on a doomed connection attempt with no retry, silently
     * leaving the relay wherever it happened to power up rather than
     * wherever the (possibly inverted) input says it should be. See that
     * function's top comment for the full story. gpio_inputs_init() above
     * still has to run this early in boot, though -- initial_status_task()
     * only waits for Wi-Fi, it assumes the input poller is already
     * sampling by the time it fires, which it always will be here (Wi-Fi
     * association+DHCP realistically takes at least several hundred ms,
     * gpio_inputs_init() runs synchronously microseconds after this
     * function starts). */

    /* We made it through the whole init sequence without hanging or
     * crashing -- if this boot is running from a freshly-flashed OTA slot
     * pending verification (see README "OTA" / partitions.csv), tell the
     * bootloader this app is good so it stops treating it as provisional.
     * ESP_ERR_NOT_SUPPORTED just means app rollback isn't compiled in
     * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) or this app wasn't pending
     * verification in the first place (e.g. a factory/first flash) --
     * both harmless, nothing to do either way. */
    esp_err_t ota_err = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_err != ESP_OK && ota_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(ota_err));
    }

    /* Boot sequence is done -- this task (and its TWDT subscription) is
     * about to go away when app_main() returns, so unsubscribe explicitly
     * rather than leaving a dangling entry nobody will ever reset again. */
    esp_task_wdt_delete(NULL);

    ESP_LOGI(TAG, "Smart Plug Control ready (%d plug(s) configured)", (int)cfg.plug_count);
}
