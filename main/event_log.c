#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "event_log.h"

static const char *TAG = "event_log";

static SemaphoreHandle_t s_lock;
static uint32_t s_max_file_size = 65536;
static uint32_t s_max_files = 5;

esp_err_t event_log_init(const system_settings_t *settings)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_max_file_size = settings->log_max_file_size ? settings->log_max_file_size : 65536;
    s_max_files = settings->log_max_files ? settings->log_max_files : 5;
    return ESP_OK;
}

void event_log_update_limits(uint32_t max_file_size, uint32_t max_files)
{
    if (max_file_size == 0 || max_files == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_max_file_size = max_file_size;
    s_max_files = max_files;
    xSemaphoreGive(s_lock);
}

static void rotated_path(char *buf, size_t buf_size, unsigned idx)
{
    snprintf(buf, buf_size, LOG_FILE_PATH ".%u", idx);
}

/* Caller must hold s_lock. */
static void rotate_locked(void)
{
    char src[64], dst[64];

    rotated_path(dst, sizeof(dst), s_max_files - 1);
    remove(dst);

    for (unsigned i = s_max_files - 1; i >= 1; i--) {
        if (i == 1) {
            strncpy(src, LOG_FILE_PATH, sizeof(src) - 1);
            src[sizeof(src) - 1] = '\0';
        } else {
            rotated_path(src, sizeof(src), i - 1);
        }
        rotated_path(dst, sizeof(dst), i);

        struct stat st;
        if (stat(src, &st) == 0) {
            remove(dst);
            rename(src, dst);
        }
        if (i == 1) break; /* unsigned underflow guard */
    }
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

void event_log_append(const char *source, const char *plug_name,
                       const char *action, bool ok, const char *detail)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    struct stat st;
    if (stat(LOG_FILE_PATH, &st) == 0 && (uint32_t)st.st_size >= s_max_file_size) {
        rotate_locked();
    }

    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open log for append");
        xSemaphoreGive(s_lock);
        return;
    }

    char line[256];
    size_t pos = 0;
    int64_t t_us = esp_timer_get_time();

    pos += snprintf(line, sizeof(line), "{\"t\":%lld,\"src\":\"", (long long)(t_us / 1000000));
    json_escape_append(line, sizeof(line), &pos, source ? source : "");
    pos += snprintf(line + pos, sizeof(line) - pos, "\",\"plug\":\"");
    json_escape_append(line, sizeof(line), &pos, plug_name ? plug_name : "");
    pos += snprintf(line + pos, sizeof(line) - pos, "\",\"action\":\"");
    json_escape_append(line, sizeof(line), &pos, action ? action : "");
    pos += snprintf(line + pos, sizeof(line) - pos, "\",\"ok\":%s,\"detail\":\"", ok ? "true" : "false");
    json_escape_append(line, sizeof(line), &pos, detail ? detail : "");
    pos += snprintf(line + pos, sizeof(line) - pos, "\"}\n");

    fwrite(line, 1, pos, f);
    fclose(f);

    xSemaphoreGive(s_lock);
}

esp_err_t event_log_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    remove(LOG_FILE_PATH);
    for (unsigned i = 1; i < s_max_files; i++) {
        char p[64];
        rotated_path(p, sizeof(p), i);
        remove(p);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
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

esp_err_t event_log_read_recent(char *out_json, size_t out_size, int max_lines)
{
    if (out_size < 3) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Gather line pointers oldest -> newest from rotated files then current. */
    #define MAX_SOURCE_FILES 8
    char *file_bufs[MAX_SOURCE_FILES] = {0};
    int n_files = 0;

    unsigned oldest_idx = (s_max_files > 1) ? (s_max_files - 1) : 0;
    for (unsigned i = oldest_idx; i >= 1 && n_files < MAX_SOURCE_FILES - 1; i--) {
        char p[64];
        rotated_path(p, sizeof(p), i);
        size_t len;
        char *b = read_whole_file(p, &len);
        if (b) file_bufs[n_files++] = b;
        if (i == 1) break;
    }
    {
        size_t len;
        char *b = read_whole_file(LOG_FILE_PATH, &len);
        if (b) file_bufs[n_files++] = b;
    }

    /* Collect all non-empty line pointers across the loaded buffers. */
    #define MAX_LINES_TOTAL 2048
    static char *all_lines[MAX_LINES_TOTAL];
    int n_lines = 0;
    for (int i = 0; i < n_files; i++) {
        char *saveptr = NULL;
        char *line = strtok_r(file_bufs[i], "\n", &saveptr);
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

    for (int i = 0; i < n_files; i++) free(file_bufs[i]);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}
