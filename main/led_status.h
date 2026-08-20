#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configures LED_STATUS_GPIO as an output and starts a dedicated task that
 * blinks it once per second (500ms on / 500ms off) for as long as the
 * firmware is alive -- a simple "the main CPU hasn't wedged" indicator that
 * doesn't depend on Wi-Fi, storage, or anything else having come up yet.
 * Call as early as possible in app_main() (no dependencies on any other
 * subsystem) so it's blinking before slower/riskier init steps run -- if
 * the device later hangs during one of those, the LED keeps blinking on its
 * own task, which tells you at a glance "CPU is fine, something else (Wi-Fi,
 * a driver call, ...) is stuck" rather than a full crash/freeze. */
esp_err_t led_status_init(void);

#ifdef __cplusplus
}
#endif
