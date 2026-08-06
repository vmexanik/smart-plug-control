#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Issues the on_url/off_url HTTP request for this plug. Writes a short
 * human-readable result (e.g. "http 200") into detail. */
esp_err_t driver_http_set(const plug_t *plug, bool on, char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
