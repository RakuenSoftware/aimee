/* td_search_render.c: see td_search_render.h. */
#include "td_search_render.h"
#include "dstr.h"

#include <stdio.h>
#include <string.h>

char *td_render_search_hits(const cJSON *hits, const char *query)
{
   dstr_t d;
   dstr_init(&d);

   int n = cJSON_IsArray(hits) ? cJSON_GetArraySize(hits) : 0;
   if (n <= 0)
   {
      dstr_appendf(&d, "No knowledge-base results for \"%s\".", query ? query : "");
      return dstr_steal(&d);
   }

   dstr_appendf(&d, "Knowledge-base results for \"%s\" (%d):\n", query ? query : "", n);
   int i = 0;
   const cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      i++;
      const cJSON *aid = cJSON_GetObjectItemCaseSensitive(h, "artifact_id");
      const cJSON *sc = cJSON_GetObjectItemCaseSensitive(h, "score");
      const cJSON *ex = cJSON_GetObjectItemCaseSensitive(h, "excerpt");
      dstr_appendf(&d, "%d. %s", i,
                   (cJSON_IsString(aid) && aid->valuestring[0]) ? aid->valuestring : "(unknown)");
      if (cJSON_IsNumber(sc))
         dstr_appendf(&d, " (score %.3f)", sc->valuedouble);
      dstr_append_str(&d, "\n");
      if (cJSON_IsString(ex) && ex->valuestring[0])
      {
         char buf[321];
         snprintf(buf, sizeof(buf), "%.320s", ex->valuestring);
         dstr_append_str(&d, "   ");
         dstr_append_str(&d, buf);
         dstr_append_str(&d, "\n");
      }
   }
   return dstr_steal(&d);
}

int td_extract_hit_docs(const cJSON *hits, int64_t *ids, const char **snips, int max)
{
   if (!cJSON_IsArray(hits) || !ids || !snips || max <= 0)
      return 0;
   int cn = 0;
   const cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      if (cn >= max)
         break;
      const cJSON *did = cJSON_GetObjectItemCaseSensitive(h, "doc_id");
      const cJSON *ex = cJSON_GetObjectItemCaseSensitive(h, "excerpt");
      if (cJSON_IsNumber(did) && did->valuedouble > 0)
      {
         ids[cn] = (int64_t)did->valuedouble;
         snips[cn] = cJSON_IsString(ex) ? ex->valuestring : "";
         cn++;
      }
   }
   return cn;
}
