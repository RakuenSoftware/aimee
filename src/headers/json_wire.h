#ifndef AIMEE_JSON_WIRE_H
#define AIMEE_JSON_WIRE_H

#include <stddef.h>
#include <string.h>
#include "cJSON.h"

/* Walk an already validated JSON document and its parsed tree in wire order.
 * cJSON uses doubles; retain large integer tokens as raw JSON for transports
 * which must round-trip IDs exactly. Strings (including escaped quotes) are
 * skipped, so digits in content or object keys cannot be mistaken for IDs. */
static inline int json_wire_preserve_integer_tokens(cJSON *node, const char **cursor)
{
   if (cJSON_IsNumber(node))
   {
      const char *p = *cursor;
      while (*p)
      {
         if (*p == '"')
         {
            p++;
            while (*p && *p != '"')
            {
               if (*p == '\\' && p[1])
                  p++;
               p++;
            }
            if (*p)
               p++;
         }
         else if (*p == '-' || (*p >= '0' && *p <= '9'))
            break;
         else
            p++;
      }
      if (!*p)
         return -1;
      const char *start = p;
      int integer = 1;
      while (*p && strchr("-+0123456789.eE", *p))
      {
         if (*p == '.' || *p == 'e' || *p == 'E')
            integer = 0;
         p++;
      }
      *cursor = p;
      if (integer &&
          (node->valuedouble >= 9007199254740992.0 || node->valuedouble <= -9007199254740992.0))
      {
         size_t length = (size_t)(p - start);
         char *token = cJSON_malloc(length + 1);
         if (!token)
            return -1;
         memcpy(token, start, length);
         token[length] = 0;
         node->type = cJSON_Raw;
         node->valuestring = token;
      }
   }
   for (cJSON *child = node->child; child; child = child->next)
      if (json_wire_preserve_integer_tokens(child, cursor) != 0)
         return -1;
   return 0;
}

static inline cJSON *json_wire_parse_exact_integers(const char *wire)
{
   cJSON *parsed = cJSON_ParseWithOpts(wire, NULL, 1);
   const char *cursor = wire;
   if (parsed && json_wire_preserve_integer_tokens(parsed, &cursor) != 0)
   {
      cJSON_Delete(parsed);
      return NULL;
   }
   return parsed;
}

/* cJSON exposes decoded strings as NUL-terminated values, so an escaped JSON
 * NUL would otherwise make distinct wire values indistinguishable. This scan
 * runs before decoding. It deliberately skips escaped backslashes: the JSON
 * text "\\\\u0000" represents the literal characters "\\u0000", not NUL. */
static inline int json_wire_has_nul_escape(const char *s, size_t n)
{
   int in_string = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (!in_string)
      {
         if (s[i] == '"')
            in_string = 1;
      }
      else if (s[i] == '"')
         in_string = 0;
      else if (s[i] == '\\' && i + 1 < n)
      {
         if (s[i + 1] == 'u' && i + 5 < n && !memcmp(s + i + 2, "0000", 4))
            return 1;
         i++;
      }
   }
   return 0;
}

#endif
