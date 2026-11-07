/* Portable exact integer encoding for JSON wire contracts. */
#ifndef AIMEE_JSON_INT64_H
#define AIMEE_JSON_INT64_H

#include "cJSON.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

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

/* Vendored parser numbers retain their original token. Accept a canonical i64
 * token only while it still describes this node's numeric value. Constructed
 * unsafe doubles have no token and remain refused by the portable reader above.
 * This reader never recovers an original integer from an unsafe constructed double. */
static inline int jo_read_i64_token_exact(const cJSON *value, int64_t *out)
{
   if (!out)
      return 0;
   if (!cJSON_IsNumber(value) || !value->number_lexeme)
      return jo_read_i64_exact(value, out);
   cJSON token = {0};
   token.type = cJSON_Raw;
   token.valuestring = value->number_lexeme;
   int64_t parsed;
   if (!jo_read_i64_exact(&token, &parsed))
      return 0;
   char *end = NULL;
   double number = strtod(value->number_lexeme, &end);
   if (!end || *end || !isfinite(number) || number != value->valuedouble ||
       (number == 0.0 && signbit(number) != signbit(value->valuedouble)))
      return 0;
   *out = parsed;
   return 1;
}

#endif
