/* sweep_parse.c: parse a proposer model response into seam candidates (pure).
 * See headers/sweep.h. */
#include "sweep.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* Tolerate a ```/```json fence: try bare parse, else the {...} substring. */
static cJSON *parse_lenient(const char *text)
{
   if (!text)
      return NULL;
   cJSON *root = cJSON_Parse(text);
   if (root)
      return root;
   const char *a = strchr(text, '{');
   const char *b = strrchr(text, '}');
   if (!a || !b || b < a)
      return NULL;
   size_t len = (size_t)(b - a + 1);
   char *buf = malloc(len + 1);
   if (!buf)
      return NULL;
   memcpy(buf, a, len);
   buf[len] = '\0';
   root = cJSON_Parse(buf);
   free(buf);
   return root;
}

int sweep_parse_candidates(const char *json_text, sweep_candidate_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   cJSON *root = parse_lenient(json_text);
   if (!root)
      return -1;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "candidates");
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(root);
      return -1;
   }
   int n = 0;
   cJSON *it;
   cJSON_ArrayForEach(it, arr)
   {
      if (n >= max)
         break;
      const cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "seam_file");
      const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "seam_symbol");
      const cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "claimed_callers");
      const cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "rationale");
      if (!cJSON_IsString(f) || !f->valuestring[0] || !cJSON_IsString(s) || !s->valuestring[0])
         continue; /* must name a concrete seam */
      sweep_candidate_t *o = &out[n];
      memset(o, 0, sizeof(*o));
      snprintf(o->seam_file, sizeof(o->seam_file), "%s", f->valuestring);
      snprintf(o->seam_symbol, sizeof(o->seam_symbol), "%s", s->valuestring);
      o->claimed_callers = cJSON_IsNumber(c) && c->valueint > 0 ? c->valueint : 0;
      if (cJSON_IsString(r))
         snprintf(o->rationale, sizeof(o->rationale), "%s", r->valuestring);
      n++;
   }
   cJSON_Delete(root);
   return n;
}
