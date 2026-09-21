/* Portable exact integer encoding for JSON wire contracts. */
#ifndef AIMEE_JSON_INT64_H
#define AIMEE_JSON_INT64_H

#include "cJSON.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

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
   /* Raw integer nodes are produced only by the exact wire parser, before
    * any double conversion can lose the original token. */
   const char *text = cJSON_IsRaw(value) ? value->valuestring : cJSON_GetStringValue(value);
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

#endif
