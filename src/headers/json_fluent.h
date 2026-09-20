/* json_fluent.h: concise helpers for cJSON request parsing and response
 * building. Used across render/server/cmd modules to strip the repeated
 * `cJSON_IsString(x) ? x->valuestring : defval` and
 * `cJSON_AddStringToObject(o, k, v ? v : "")` boilerplate. */
#ifndef DEC_JSON_FLUENT_H
#define DEC_JSON_FLUENT_H 1

#include "cJSON.h"
#include <stdint.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <inttypes.h>

/* Lossless integer transport through cJSON's double-backed number nodes.
 * Preserve compatible numeric values within the safe integer range; use canonical
 * decimal strings outside it. A number outside that range may already be rounded
 * and is therefore refused by the reader. Failure leaves *out untouched. */
static inline cJSON *jo_i64_value_exact(int64_t value)
{
   if (value >= -INT64_C(9007199254740991) && value <= INT64_C(9007199254740991))
      return cJSON_CreateNumber((double)value);
   char text[32];
   snprintf(text, sizeof(text), "%" PRId64, value);
   return cJSON_CreateString(text);
}

static inline int jo_read_i64_exact(const cJSON *value, int64_t *out)
{
   if (!out)
      return 0;
   if (cJSON_IsNumber(value))
   {
      double number = value->valuedouble;
      if (!isfinite(number) || number < -9007199254740991.0 || number > 9007199254740991.0 ||
          (double)(int64_t)number != number)
         return 0;
      *out = (int64_t)number;
      return 1;
   }
   const char *text = cJSON_GetStringValue(value);
   if (!text || !text[0])
      return 0;
   int negative = text[0] == '-';
   const char *digits = text + negative;
   if (!digits[0] || (digits[0] == '0' && (digits[1] || negative)))
      return 0;
   uint64_t limit = (uint64_t)INT64_MAX + (unsigned)negative, parsed = 0;
   for (const char *p = digits; *p; p++)
   {
      if (*p < '0' || *p > '9')
         return 0;
      unsigned digit = (unsigned)(*p - '0');
      if (parsed > (limit - digit) / 10)
         return 0;
      parsed = parsed * 10 + digit;
   }
   *out = negative ? (parsed == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)parsed)
                   : (int64_t)parsed;
   return 1;
}

/* ---- Optional getters with defaults ---- */
/* All tolerate NULL obj. Return defval if the key is missing or has the wrong
 * JSON type. Strings are not copied — borrowed from the parent cJSON value, so
 * only valid while the parent is alive. */
const char *jo_str(cJSON *obj, const char *key, const char *defval);
int jo_int(cJSON *obj, const char *key, int defval);
int64_t jo_i64(cJSON *obj, const char *key, int64_t defval);
double jo_num(cJSON *obj, const char *key, double defval);
int jo_bool(cJSON *obj, const char *key, int defval);

/* Const-correct "string field or empty": the value at `key` if it is a non-empty
 * string, else "" (never NULL). For read-only payload/record JSON access. */
static inline const char *jo_cstr(const cJSON *obj, const char *key)
{
   const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
   return (cJSON_IsString(j) && j->valuestring[0]) ? j->valuestring : "";
}

/* Human-readable JSON type name for `item` (string/number/boolean/array/object/
 * null, else "unknown"). For "expected X, got %s" config-validation diagnostics.
 * Inline so it adds no link-time dependency to the many TUs that include this. */
static inline const char *jo_type_name(const cJSON *item)
{
   if (cJSON_IsString(item))
      return "string";
   if (cJSON_IsNumber(item))
      return "number";
   if (cJSON_IsBool(item))
      return "boolean";
   if (cJSON_IsArray(item))
      return "array";
   if (cJSON_IsObject(item))
      return "object";
   if (cJSON_IsNull(item))
      return "null";
   return "unknown";
}

/* Decode bounded numeric arrays atomically: an invalid member leaves output
 * untouched. Zero denotes an empty or invalid array. */
static inline int jo_float_array(const cJSON *array, float *out, int capacity)
{
   int n = cJSON_GetArraySize(array);
   if (!cJSON_IsArray(array) || !out || n <= 0 || n > capacity)
      return 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, array) if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
                                       fabs(item->valuedouble) > FLT_MAX) return 0;
   int i = 0;
   cJSON_ArrayForEach(item, array) out[i++] = (float)item->valuedouble;
   return n;
}

static inline int jo_float_matrix(const cJSON *array, float *out, int rows, int columns)
{
   if (!cJSON_IsArray(array) || !out || rows <= 0 || columns <= 0 ||
       cJSON_GetArraySize(array) != rows ||
       (size_t)rows > SIZE_MAX / sizeof(float) / (size_t)columns)
      return 0;
   const cJSON *row, *item;
   cJSON_ArrayForEach(row, array)
   {
      if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != columns)
         return 0;
      cJSON_ArrayForEach(item, row) if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
                                        fabs(item->valuedouble) > FLT_MAX) return 0;
   }
   size_t i = 0;
   cJSON_ArrayForEach(row, array) cJSON_ArrayForEach(item, row) out[i++] = (float)item->valuedouble;
   return rows;
}

/* ---- Required getters. Return 0 on success, -1 on missing/wrong type. ---- */
int jo_need_str(cJSON *obj, const char *key, const char **out);
int jo_need_num(cJSON *obj, const char *key, double *out);

/* ---- NUL-safe add helpers: treat NULL strings as "". ---- */
void jo_add_str(cJSON *obj, const char *key, const char *val);
void jo_add_i64(cJSON *obj, const char *key, int64_t val);
void jo_add_num(cJSON *obj, const char *key, double val);
void jo_add_bool(cJSON *obj, const char *key, int val);

/* ---- Response-object builders. Ownership transfers to caller. ---- */
cJSON *jo_ok(void);                                /* {"status":"ok"} */
cJSON *jo_ok_kv(const char *key, const char *val); /* ok + one string field */
cJSON *jo_err(const char *message);                /* {"status":"error","message":msg} */

/* Add a string to a cJSON object with NUL-safety.
 * Wraps cJSON_AddStringToObject so NULL values become "" rather than crashing. */
#define JSON_ADD_STR(obj, key, val) cJSON_AddStringToObject((obj), (key), (val) ? (val) : "")

#endif /* DEC_JSON_FLUENT_H */
