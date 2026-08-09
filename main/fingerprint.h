#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol/model detection cascade for a single HTTP host, per the scoped-
 * down version of the discovery spec (see README "Автопошук у мережі" for
 * what was and wasn't carried over). Given an IP known to have something on
 * TCP:80, tries in order:
 *   1. GET /shelly                    -> Shelly Gen1 or Gen2+/Gen3
 *   2. GET /cm?cmnd=Status%200         -> Tasmota
 *   3. GET /                           -> ESPHome heuristic (if enabled)
 * Stops at the first family that answers. Does not perform any control
 * action -- only classifies and, where possible, fills in the on/off URL
 * template that driver_http.c would need to actually operate the plug. */

typedef struct {
    plug_family_t family;
    plug_kind_t kind;
    char model[32];        /* e.g. "SHPLG-S", "S3PL-30110EU", "Gosund SP111", "" if unknown */
    bool needs_auth;        /* got 401 during the cascade -- device has a password set */
    char on_url[URL_LEN];   /* empty if the family/model couldn't be mapped to a template */
    char off_url[URL_LEN];
    bool use_post;
} plug_fingerprint_t;

/* Runs the cascade against http://ip/ (channel 0/1, i.e. the first/only
 * switch -- see README for the multi-channel-device caveat). Always returns
 * ESP_OK and fills *out (family/kind left at *_UNKNOWN, urls empty, if
 * nothing matched) -- callers should not treat a non-match as an error. */
esp_err_t fingerprint_probe(const char *ip, plug_fingerprint_t *out);

const char *plug_family_label(plug_family_t f);
const char *plug_kind_label(plug_kind_t k);

#ifdef __cplusplus
}
#endif
