#include <ctype.h>
#include "db2_rel_seed.h"
#include "db2_rel_type_helpers.h"
#include <string.h>

enum { DB2_NODE_OTHER = 99 };

void rel_type_normalize(const char *in, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   size_t o = 0;
   int prev_us = 1;
   int prev_lower_or_digit = 0;
   for (const char *p = in ? in : ""; *p && o + 1 < out_len; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c))
      {
         if (isupper(c) && prev_lower_or_digit && !prev_us && o + 1 < out_len)
            out[o++] = '_';
         if (o + 1 < out_len)
            out[o++] = (char)tolower(c);
         prev_us = 0;
         prev_lower_or_digit = (islower(c) || isdigit(c));
      }
      else if (!prev_us)
      {
         out[o++] = '_';
         prev_us = 1;
         prev_lower_or_digit = 0;
      }
   }
   while (o > 0 && out[o - 1] == '_')
      o--;
   out[o] = '\0';
}

int rel_type_is_functional(const char *rel_type)
{
   if (!rel_type)
      return 0;
   static const char *const functional[] = {
       "lives_in", "born_in",   "age",      "located_in",    "has_hostname",
       "spouse",   "works_for", "has_role", "device_has_ip",
   };
   for (size_t i = 0; i < sizeof(functional) / sizeof(functional[0]); i++)
      if (strcmp(rel_type, functional[i]) == 0)
         return 1;
   return 0;
}

int rel_type_kind_allowed(const db2_rel_seed_def_t *def, int is_head, int kind)
{
   if (!def)
      return 0;
   const int *list = is_head ? def->head_kinds : def->tail_kinds;
   int n = is_head ? def->head_kind_count : def->tail_kind_count;
   for (int i = 0; i < n; i++)
      if (list[i] == DB2_NODE_OTHER || list[i] == kind)
         return 1;
   return 0;
}
