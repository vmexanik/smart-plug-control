#pragma once
/*
 * Minimal cJSON-API-compatible JSON library, vendored directly into this
 * project.
 *
 * Why: ESP-IDF v6 dropped the built-in "json" (cJSON) component that
 * earlier IDF versions bundled at components/json/cJSON -- confirmed by
 * diffing .gitmodules between IDF releases, the cJSON submodule is gone
 * as of v6.0.1. Rather than guess at whatever managed-component name
 * Espressif may or may not publish cJSON under on the component registry,
 * this file implements the small subset of the classic cJSON API that
 * this firmware actually uses (see storage.c / web_server.c / event_log.h),
 * so the project has zero dependency on that component's fate.
 *
 * API names/semantics intentionally match github.com/DaveGamble/cJSON so
 * this can be swapped for the real library later with no code changes
 * elsewhere, if desired.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int cJSON_bool;

#define cJSON_Invalid 0
#define cJSON_False   1
#define cJSON_True    2
#define cJSON_NULL    3
#define cJSON_Number  4
#define cJSON_String  5
#define cJSON_Array   6
#define cJSON_Object  7

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;

    int type;

    char *valuestring;
    int valueint;
    double valuedouble;

    char *string; /* key, if this item is a member of an object */
} cJSON;

/* ---- construction ---- */
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateBool(cJSON_bool b);

cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item);

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, cJSON_bool b);

/* ---- access ---- */
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);

cJSON_bool cJSON_IsString(const cJSON *item);
cJSON_bool cJSON_IsBool(const cJSON *item);
cJSON_bool cJSON_IsTrue(const cJSON *item);
cJSON_bool cJSON_IsNumber(const cJSON *item);
cJSON_bool cJSON_IsArray(const cJSON *item);

#define cJSON_ArrayForEach(element, array) \
    for ((element) = ((array) != NULL) ? (array)->child : NULL; (element) != NULL; (element) = (element)->next)

/* ---- parse / print / free ---- */
cJSON *cJSON_Parse(const char *value);
char *cJSON_PrintUnformatted(const cJSON *item);
void cJSON_Delete(cJSON *item);

#ifdef __cplusplus
}
#endif
