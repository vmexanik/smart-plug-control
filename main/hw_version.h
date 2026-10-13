#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware revision this firmware is built for (menuconfig -> Smart Plug
 * Control -> Hardware revision, default "1.1"). The fallback keeps the build
 * working if an old sdkconfig has not been regenerated yet. */
#ifndef CONFIG_HW_VERSION
#define CONFIG_HW_VERSION "1.1"
#endif

/* The revision is stamped into the image right after esp_app_desc_t (section
 * .rodata_custom_desc, see hw_version.c), i.e. inside the first 4 KB of the
 * .bin that the OTA handler already reads. Record layout: the ASCII tag
 * (no NUL) followed by a NUL-terminated revision string in a fixed field. */
#define HW_REV_TAG          "SPC-HW-REV:"
#define HW_REV_TAG_LEN      (sizeof(HW_REV_TAG) - 1)
#define HW_REV_VER_MAX      16

typedef struct __attribute__((packed)) {
    /* Deliberately not NUL-terminated (exact fit): the tag is only ever
     * compared with memcmp(). nonstring silences GCC's
     * -Wunterminated-string-initialization, which IDF builds with -Werror. */
    char tag[HW_REV_TAG_LEN] __attribute__((nonstring));
    char ver[HW_REV_VER_MAX];
} hw_rev_desc_t;

/* Offset of the record in an app image: 24-byte image header + 8-byte first
 * segment header + 256-byte esp_app_desc_t. Scanned over a small window so a
 * linker padding change cannot silently disable the check. */
#define HW_REV_SCAN_START   (24 + 8 + 256)
#define HW_REV_SCAN_WINDOW  64

/* Revision of the running firmware, e.g. "1.1". */
const char *hw_version_str(void);

/* Extracts the hardware revision from the first bytes of an app image.
 * Returns false (out = "") if the image carries no record -- e.g. firmware
 * built before this check existed. */
bool hw_rev_from_image(const uint8_t *img, size_t len, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
