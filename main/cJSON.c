#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include "cJSON.h"

/* ------------------------------------------------------------------ */
/* construction / linking                                              */
/* ------------------------------------------------------------------ */

static cJSON *new_item(int type)
{
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    if (item) item->type = type;
    return item;
}

static char *dupstr(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

/* append item to the end of parent's child list (siblings linked via next) */
static void link_child(cJSON *parent, cJSON *item)
{
    if (!parent || !item) return;
    if (!parent->child) {
        parent->child = item;
    } else {
        cJSON *c = parent->child;
        while (c->next) c = c->next;
        c->next = item;
        item->prev = c;
    }
}

cJSON *cJSON_CreateObject(void) { return new_item(cJSON_Object); }
cJSON *cJSON_CreateArray(void)  { return new_item(cJSON_Array); }

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = new_item(cJSON_String);
    if (item) item->valuestring = dupstr(string ? string : "");
    return item;
}

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = new_item(cJSON_Number);
    if (item) {
        item->valuedouble = num;
        item->valueint = (int)num;
    }
    return item;
}

cJSON *cJSON_CreateBool(cJSON_bool b)
{
    return new_item(b ? cJSON_True : cJSON_False);
}

cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    if (!array || !item) return 0;
    link_child(array, item);
    return 1;
}

cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (!object || !item) return 0;
    free(item->string);
    item->string = dupstr(string);
    link_child(object, item);
    return 1;
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string)
{
    cJSON *item = cJSON_CreateString(string);
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number)
{
    cJSON *item = cJSON_CreateNumber(number);
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, cJSON_bool b)
{
    cJSON *item = cJSON_CreateBool(b);
    if (item) cJSON_AddItemToObject(object, name, item);
    return item;
}

/* ------------------------------------------------------------------ */
/* access                                                               */
/* ------------------------------------------------------------------ */

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    if (!object || !string) return NULL;
    cJSON *c = object->child;
    while (c) {
        if (c->string && strcmp(c->string, string) == 0) return c;
        c = c->next;
    }
    return NULL;
}

cJSON_bool cJSON_IsString(const cJSON *item) { return item && item->type == cJSON_String; }
cJSON_bool cJSON_IsBool(const cJSON *item)   { return item && (item->type == cJSON_True || item->type == cJSON_False); }
cJSON_bool cJSON_IsTrue(const cJSON *item)   { return item && item->type == cJSON_True; }
cJSON_bool cJSON_IsNumber(const cJSON *item) { return item && item->type == cJSON_Number; }
cJSON_bool cJSON_IsArray(const cJSON *item)  { return item && item->type == cJSON_Array; }

/* ------------------------------------------------------------------ */
/* delete                                                               */
/* ------------------------------------------------------------------ */

void cJSON_Delete(cJSON *item)
{
    while (item) {
        cJSON *next = item->next;
        if (item->child) cJSON_Delete(item->child);
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}

/* ------------------------------------------------------------------ */
/* parsing                                                              */
/* ------------------------------------------------------------------ */

static void skip_ws(const char **s)
{
    while (**s && isspace((unsigned char)**s)) (*s)++;
}

/* utf-8 encode a unicode codepoint into buf, returns bytes written */
static int utf8_encode(unsigned long cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return -1;
    }
    return v;
}

/* *s must point at the opening quote. Returns malloc'd unescaped string,
 * advances *s past the closing quote. NULL on malformed input. */
static char *parse_string_raw(const char **s)
{
    const char *p = *s + 1; /* skip opening quote */
    size_t cap = 16, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    while (*p && *p != '"') {
        char c = *p;
        char enc[4];
        int enc_len = 0;

        if (c == '\\') {
            p++;
            if (!*p) goto fail;
            switch (*p) {
                case '"':  enc[0] = '"';  enc_len = 1; break;
                case '\\': enc[0] = '\\'; enc_len = 1; break;
                case '/':  enc[0] = '/';  enc_len = 1; break;
                case 'b':  enc[0] = '\b'; enc_len = 1; break;
                case 'f':  enc[0] = '\f'; enc_len = 1; break;
                case 'n':  enc[0] = '\n'; enc_len = 1; break;
                case 'r':  enc[0] = '\r'; enc_len = 1; break;
                case 't':  enc[0] = '\t'; enc_len = 1; break;
                case 'u': {
                    int cp = hex4(p + 1);
                    if (cp < 0) goto fail;
                    p += 4;
                    enc_len = utf8_encode((unsigned long)cp, enc);
                    break;
                }
                default:
                    goto fail;
            }
            p++;
        } else {
            enc[0] = c;
            enc_len = 1;
            p++;
        }

        if (len + (size_t)enc_len + 1 > cap) {
            cap = (cap + enc_len) * 2;
            char *grown = (char *)realloc(out, cap);
            if (!grown) goto fail;
            out = grown;
        }
        memcpy(out + len, enc, (size_t)enc_len);
        len += (size_t)enc_len;
    }

    if (*p != '"') goto fail;
    out[len] = '\0';
    *s = p + 1;
    return out;

fail:
    free(out);
    return NULL;
}

static cJSON *parse_value(const char **s);

static cJSON *parse_array(const char **s)
{
    cJSON *arr = cJSON_CreateArray();
    (*s)++; /* skip '[' */
    skip_ws(s);
    if (**s == ']') { (*s)++; return arr; }

    for (;;) {
        skip_ws(s);
        cJSON *val = parse_value(s);
        if (!val) goto fail;
        link_child(arr, val);
        skip_ws(s);
        if (**s == ',') { (*s)++; continue; }
        if (**s == ']') { (*s)++; break; }
        goto fail;
    }
    return arr;

fail:
    cJSON_Delete(arr);
    return NULL;
}

static cJSON *parse_object(const char **s)
{
    cJSON *obj = cJSON_CreateObject();
    (*s)++; /* skip '{' */
    skip_ws(s);
    if (**s == '}') { (*s)++; return obj; }

    for (;;) {
        skip_ws(s);
        if (**s != '"') goto fail;
        char *key = parse_string_raw(s);
        if (!key) goto fail;
        skip_ws(s);
        if (**s != ':') { free(key); goto fail; }
        (*s)++;
        skip_ws(s);
        cJSON *val = parse_value(s);
        if (!val) { free(key); goto fail; }
        val->string = key;
        link_child(obj, val);
        skip_ws(s);
        if (**s == ',') { (*s)++; continue; }
        if (**s == '}') { (*s)++; break; }
        goto fail;
    }
    return obj;

fail:
    cJSON_Delete(obj);
    return NULL;
}

static cJSON *parse_value(const char **s)
{
    skip_ws(s);
    const char *p = *s;

    if (strncmp(p, "null", 4) == 0) { *s += 4; return new_item(cJSON_NULL); }
    if (strncmp(p, "true", 4) == 0) { *s += 4; cJSON *i = new_item(cJSON_True); if (i) i->valueint = 1; return i; }
    if (strncmp(p, "false", 5) == 0) { *s += 5; return new_item(cJSON_False); }

    if (*p == '"') {
        char *str = parse_string_raw(s);
        if (!str) return NULL;
        cJSON *item = new_item(cJSON_String);
        if (!item) { free(str); return NULL; }
        item->valuestring = str;
        return item;
    }

    if (*p == '[') return parse_array(s);
    if (*p == '{') return parse_object(s);

    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end = NULL;
        double d = strtod(p, &end);
        if (end == p) return NULL;
        *s = end;
        cJSON *item = new_item(cJSON_Number);
        if (item) {
            item->valuedouble = d;
            item->valueint = (int)d;
        }
        return item;
    }

    return NULL;
}

cJSON *cJSON_Parse(const char *value)
{
    if (!value) return NULL;
    const char *s = value;
    cJSON *result = parse_value(&s);
    if (!result) return NULL;
    skip_ws(&s);
    /* trailing garbage is tolerated, mirroring cJSON_Parse (not _ParseWithOpts) */
    return result;
}

/* ------------------------------------------------------------------ */
/* printing                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strbuf;

static int sb_ensure(strbuf *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap) return 1;
    size_t newcap = sb->cap ? sb->cap * 2 : 64;
    while (newcap < sb->len + extra + 1) newcap *= 2;
    char *grown = (char *)realloc(sb->buf, newcap);
    if (!grown) return 0;
    sb->buf = grown;
    sb->cap = newcap;
    return 1;
}

static void sb_append(strbuf *sb, const char *s, size_t n)
{
    if (!sb_ensure(sb, n)) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append_str(strbuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }
static void sb_append_char(strbuf *sb, char c) { sb_append(sb, &c, 1); }

static void print_string(strbuf *sb, const char *s)
{
    sb_append_char(sb, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"':  sb_append_str(sb, "\\\""); break;
                case '\\': sb_append_str(sb, "\\\\"); break;
                case '\n': sb_append_str(sb, "\\n"); break;
                case '\r': sb_append_str(sb, "\\r"); break;
                case '\t': sb_append_str(sb, "\\t"); break;
                case '\b': sb_append_str(sb, "\\b"); break;
                case '\f': sb_append_str(sb, "\\f"); break;
                default:
                    if (*p < 0x20) {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                        sb_append_str(sb, tmp);
                    } else {
                        sb_append_char(sb, (char)*p);
                    }
            }
        }
    }
    sb_append_char(sb, '"');
}

static void print_number(strbuf *sb, double d)
{
    char tmp[32];
    if (d == (double)(long long)d && fabs(d) < 1e15) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)d);
    } else {
        snprintf(tmp, sizeof(tmp), "%.17g", d);
    }
    sb_append_str(sb, tmp);
}

static void print_value(strbuf *sb, const cJSON *item)
{
    switch (item->type) {
        case cJSON_NULL:  sb_append_str(sb, "null"); break;
        case cJSON_False: sb_append_str(sb, "false"); break;
        case cJSON_True:  sb_append_str(sb, "true"); break;
        case cJSON_Number: print_number(sb, item->valuedouble); break;
        case cJSON_String: print_string(sb, item->valuestring); break;
        case cJSON_Array: {
            sb_append_char(sb, '[');
            for (cJSON *c = item->child; c; c = c->next) {
                print_value(sb, c);
                if (c->next) sb_append_char(sb, ',');
            }
            sb_append_char(sb, ']');
            break;
        }
        case cJSON_Object: {
            sb_append_char(sb, '{');
            for (cJSON *c = item->child; c; c = c->next) {
                print_string(sb, c->string ? c->string : "");
                sb_append_char(sb, ':');
                print_value(sb, c);
                if (c->next) sb_append_char(sb, ',');
            }
            sb_append_char(sb, '}');
            break;
        }
        default: break;
    }
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    if (!item) return NULL;
    strbuf sb = {0};
    if (!sb_ensure(&sb, 1)) return NULL;
    sb.buf[0] = '\0';
    print_value(&sb, item);
    return sb.buf;
}
