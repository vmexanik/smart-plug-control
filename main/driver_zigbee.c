#include <stdio.h>
#include "driver_zigbee.h"

esp_err_t driver_zigbee_set(const plug_t *plug, bool on, char *detail, size_t detail_size)
{
    (void)plug;
    (void)on;
    snprintf(detail, detail_size, "zigbee driver not implemented yet (reserved for future hardware)");
    return ESP_ERR_NOT_SUPPORTED;
}
