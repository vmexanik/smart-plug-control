#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "event_log.h"

static const char *TAG = "event_log";

/* Fixed cap on the number of entries kept, replacing the old byte-size +
 * multi-file rotation scheme entirely (no more log.txt.1..N, no more
 * log_max_file_size/log_max_files settings -- see event_log.h and the
 * README/task history for why: the rename-based rotation in rotate_locked()
 * was the suspected cause of a device hanging after roughly a day of
 * uptime on first power-up (fine again after a manual reboot -- consistent
 * with something going wrong the first time flash/LittleFS accumulated
 * enough rotated files to actually trigger a rotation, then staying fine
 * because the freshly-rebooted log started small again). A single
 * fixed-size file that just keeps its N most recent lines needs no
 * rename/multi-file bookkeeping at all. 50 is a plain constant, not a
 * setting -- nothing to get wrong from the web UI here any more. */
#define EVENT_LOG_MAX_LINES 50

static SemaphoreHandle_t s_lock;

/* One-time cleanup of rotated files left behind by older firmware
 * (log.txt.1 .. log.txt.8, see the removed rotate_locked()/MAX_SOURCE_FILES
 * scheme) so they don't sit on flash forever after an OTA update from a
 * version that used them. Safe to call even if none exist -- remove() on a
 * missing file is a harmless no-op. */
static void remove_legacy_rotated_files(void)
{
    for (unsigned i = 1; i <= 8; i++) {
        char p[64];
        snprintf(p, sizeof(p), LOG_FILE_PATH ".%u", i);
        remove(p);
    }
}

esp_err_t event_log_init(const system_settings_t *settings)
{
    (void)settings; /* no more log_max_file_size/log_max_files to read */
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    remove_legacy_rotated_files();
    return ESP_OK;
}

/* Appends fixed JSON syntax (not user data -- json_escape_append() below
 * handles that) to dst, clamping pos so it can never run past dst_size.
 *
 * The pattern this replaces -- `pos += snprintf(dst + pos, dst_size - pos,
 * fmt, ...)` -- is NOT safe: snprintf() returns the length it *would* have
 * written had the buffer been big enough, not what actually fit. If an
 * earlier field (plug name, detail -- both attacker/user-controlled length,
 * detail especially since it can now carry ~200-byte Cyrillic messages, see
 * do_dispatch()'s `char detail[200]`) has already pushed pos close to
 * dst_size, that add-the-wanted-not-the-written-length arithmetic can push
 * pos *past* dst_size. The next call then computes `dst_size - pos` as an
 * unsigned (size_t) subtraction that wraps to a huge number, so snprintf
 * believes it has gigabytes of room -- and even without that wraparound,
 * the final `fwrite(line, 1, pos, f)` reads `pos` bytes from a buffer only
 * dst_size bytes long, i.e. an out-of-bounds stack read landing garbage
 * bytes into the log file, which can corrupt that line's JSON badly enough
 * to break event_log_read_recent()'s whole response (it just concatenates
 * raw lines with commas -- one bad line invalidates the entire array). */
static size_t append_fmt(char *dst, size_t dst_size, size_t pos, const char *fmt, ...)
{
    if (pos >= dst_size) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + pos, dst_size - pos, fmt, ap);
    va_end(ap);
    if (n <= 0) return pos;
    size_t avail = dst_size - pos; /* includes the byte for the NUL */
    size_t written = ((size_t)n < avail) ? (size_t)n : avail - 1;
    return pos + written;
}

static void json_escape_append(char *dst, size_t dst_size, size_t *pos, const char *src)
{
    if (!src) return;
    for (const char *c = src; *c && *pos < dst_size - 2; c++) {
        if (*c == '"' || *c == '\\') {
            if (*pos < dst_size - 3) { dst[(*pos)++] = '\\'; dst[(*pos)++] = *c; }
        } else if (*c == '\n') {
            if (*pos < dst_size - 3) { dst[(*pos)++] = '\\'; dst[(*pos)++] = 'n'; }
        } else if ((unsigned char)*c >= 0x20) {
            dst[(*pos)++] = *c;
        }
    }
}

static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) { fclose(f); *out_len = 0; return NULL; }
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); *out_len = 0; return NULL; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/* Caller must hold s_lock. Rewrites LOG_FILE_PATH so it holds at most the
 * last EVENT_LOG_MAX_LINES lines of whatever's currently in `lines`
 * (oldest-first). Uses the same write-to-tmp-then-rename pattern as
 * storage_save_config() -- a half-written file from a power loss mid-write
 * is not something a straight in-place rewrite could recover from. */
static void rewrite_trimmed_locked(char **lines, int n_lines)
{
    int start = (n_lines > EVENT_LOG_MAX_LINES) ? (n_lines - EVENT_LOG_MAX_LINES) : 0;

    const char *tmp_path = LOG_FILE_PATH ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for trim rewrite", tmp_path);
        return;
    }
    for (int i = start; i < n_lines; i++) {
        fputs(lines[i], f);
        fputc('\n', f);
    }
    fclose(f);

    remove(LOG_FILE_PATH);
    if (rename(tmp_path, LOG_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s into place", tmp_path);
    }
}

void event_log_append(const char *source, const char *plug_name,
                       const char *action, bool ok, const char *detail)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open log for append");
        xSemaphoreGive(s_lock);
        return;
    }

    /* 384, not 256: plug names can be up to PLUG_NAME_LEN(32) bytes and
     * detail strings up to ~200 bytes of Cyrillic (see do_dispatch()'s
     * `char detail[200]`) -- 256 left barely any room for the surrounding
     * JSON syntax once both are near their max. append_fmt()/
     * json_escape_append() both clamp to this size regardless, so this is
     * headroom for correctness of *content*, not a safety requirement (the
     * safety part is that neither helper can run pos past the buffer). */
    char line[384];
    size_t pos = 0;
    /* Real epoch seconds -- reads as 1970 until NTP syncs or the user sets
     * the clock manually via the web UI (see time_sync.c / POST /api/time). */
    time_t now = time(NULL);

    pos = append_fmt(line, sizeof(line), pos, "{\"t\":%lld,\"src\":\"", (long long)now);
    json_escape_append(line, sizeof(line), &pos, source ? source : "");
    pos = append_fmt(line, sizeof(line), pos, "\",\"plug\":\"");
    json_escape_append(line, sizeof(line), &pos, plug_name ? plug_name : "");
    pos = append_fmt(line, sizeof(line), pos, "\",\"action\":\"");
    json_escape_append(line, sizeof(line), &pos, action ? action : "");
    pos = append_fmt(line, sizeof(line), pos, "\",\"ok\":%s,\"detail\":\"", ok ? "true" : "false");
    json_escape_append(line, sizeof(line), &pos, detail ? detail : "");
    pos = append_fmt(line, sizeof(line), pos, "\"}\n");

    fwrite(line, 1, pos, f);
    fclose(f);

    /* Trim to the last EVENT_LOG_MAX_LINES lines. Reads the whole (small --
     * capped at 50 lines of at most ~384 bytes each, well under 20KB)
     * file back rather than tracking a running count, so a count that got
     * out of sync (e.g. after a crash mid-write) self-corrects on the very
     * next append instead of drifting forever. */
    size_t len;
    char *buf = read_whole_file(LOG_FILE_PATH, &len);
    if (buf) {
        char *lines[EVENT_LOG_MAX_LINES + 1];
        int n_lines = 0;
        char *saveptr = NULL;
        char *ln = strtok_r(buf, "\n", &saveptr);
        while (ln && n_lines < EVENT_LOG_MAX_LINES + 1) {
            if (*ln) lines[n_lines++] = ln;
            ln = strtok_r(NULL, "\n", &saveptr);
        }
        if (n_lines > EVENT_LOG_MAX_LINES) {
            rewrite_trimmed_locked(lines, n_lines);
        }
        free(buf);
    }

    xSemaphoreGive(s_lock);
}

esp_err_t event_log_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    remove(LOG_FILE_PATH);
    remove_legacy_rotated_files();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t event_log_read_recent(char *out_json, size_t out_size, int max_lines)
{
    if (out_size < 3) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t len;
    char *buf = read_whole_file(LOG_FILE_PATH, &len);

    #define MAX_LINES_TOTAL (EVENT_LOG_MAX_LINES + 1)
    char *all_lines[MAX_LINES_TOTAL];
    int n_lines = 0;
    if (buf) {
        char *saveptr = NULL;
        char *line = strtok_r(buf, "\n", &saveptr);
        while (line && n_lines < MAX_LINES_TOTAL) {
            if (*line) all_lines[n_lines++] = line;
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    int start = 0;
    if (max_lines > 0 && n_lines > max_lines) {
        start = n_lines - max_lines;
    }

    size_t pos = 0;
    out_json[pos++] = '[';
    bool first = true;
    for (int i = start; i < n_lines; i++) {
        size_t item_len = strlen(all_lines[i]);
        size_t needed = item_len + (first ? 0 : 1) + 1; /* +comma +closing bracket slack */
        if (pos + needed >= out_size) break;
        if (!first) out_json[pos++] = ',';
        memcpy(out_json + pos, all_lines[i], item_len);
        pos += item_len;
        first = false;
    }
    out_json[pos++] = ']';
    out_json[pos] = '\0';

    if (buf) free(buf);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}
