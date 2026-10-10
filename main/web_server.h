#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(void);

/* true while a firmware upload (OTA) is in progress. Background network
 * pollers should stay quiet in that time. */
bool web_server_ota_busy(void);

#ifdef __cplusplus
}
#endif
