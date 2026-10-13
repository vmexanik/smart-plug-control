#include <string.h>
#include "hw_version.h"

_Static_assert(sizeof(CONFIG_HW_VERSION) <= HW_REV_VER_MAX,
               "CONFIG_HW_VERSION is too long (max 15 characters)");

/* Not wrapped in KEEP() by the IDF linker script, so it must be referenced
 * (hw_version_str() below, called from web_server.c) to survive --gc-sections. */
__attribute__((used, section(".rodata_custom_desc")))
const hw_rev_desc_t hw_rev_desc = {
    .tag = HW_REV_TAG,
    .ver = CONFIG_HW_VERSION,
};

const char *hw_version_str(void)
{
    return hw_rev_desc.ver;
}

bool hw_rev_from_image(const uint8_t *img, size_t len, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!img) return false;

    const size_t rec = HW_REV_TAG_LEN + HW_REV_VER_MAX;
    for (size_t off = HW_REV_SCAN_START;
         off < HW_REV_SCAN_START + HW_REV_SCAN_WINDOW && off + rec <= len;
         off++) {
        if (memcmp(img + off, HW_REV_TAG, HW_REV_TAG_LEN) != 0) continue;

        const char *v = (const char *)img + off + HW_REV_TAG_LEN;
        size_t n = strnlen(v, HW_REV_VER_MAX);
        if (n == 0 || n >= HW_REV_VER_MAX) return false; /* empty or not NUL-terminated */
        if (n >= out_size) n = out_size - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return true;
    }
    return false;
}
