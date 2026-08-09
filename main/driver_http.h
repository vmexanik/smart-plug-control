#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Issues the on_url/off_url HTTP request for this plug. Writes a short
 * human-readable result (e.g. "http 200") into detail. */
esp_err_t driver_http_set(const plug_t *plug, bool on, char *detail, size_t detail_size);

/* Best-effort query of the device's actual current on/off state, derived
 * from plug->http.family and plug->http.on_url's host (Shelly/Tasmota only
 * -- their status shape is well known; ESPHome's varies too much per
 * device to guess reliably, and PLUG_FAMILY_UNKNOWN has no fixed shape at
 * all). Used when a plug is first added so it doesn't default to "off"
 * when it might already be on.
 *
 * Returns ESP_OK and fills *out_on only when the device answered with a
 * value we're confident we parsed correctly; ESP_ERR_NOT_SUPPORTED for an
 * unrecognized/unqueryable family, ESP_FAIL for a network error or a
 * response that didn't parse. */
esp_err_t driver_http_get_status(const plug_t *plug, bool *out_on);

/* Best-effort query of power/voltage/current for plugs whose fingerprinted
 * family exposes power metering. Family-specific endpoints:
 *   Shelly Gen1: GET /status              -> meters[0].power (W only, no V/A)
 *   Shelly Gen2+: GET /rpc/Switch.GetStatus?id=0 -> apower/voltage/current
 *   Tasmota:      GET /cm?cmnd=Status8     -> StatusSNS.ENERGY.{Power,Voltage,Current}
 * ESPHome and PLUG_FAMILY_UNKNOWN return ESP_ERR_NOT_SUPPORTED immediately,
 * same reasoning as driver_http_get_status().
 *
 * Returns ESP_OK whenever the device answered and the body parsed as valid
 * JSON for its family -- *out->valid then tells whether it actually reports
 * metering data (false for a non-power-metering device of that family,
 * e.g. a plain Shelly1 relay). ESP_FAIL on network error / bad HTTP status /
 * unparseable body. */
esp_err_t driver_http_get_metrics(const plug_t *plug, plug_metrics_t *out);

#ifdef __cplusplus
}
#endif
