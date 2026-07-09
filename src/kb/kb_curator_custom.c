/* kb_curator_custom.c: parse + validate composed (user-defined) curator stages.
 * See header. Deliberately dependency-light (cJSON + strings only) so it is
 * unit-testable with a mock base resolver. */
#include "kb_curator_custom.h"

#include <string.h>

#include "cJSON.h"

/* A stage name is a stable id used in logs, the GUI, and stage_order — restrict
 * it to a safe identifier charset so a hostile config can't inject control chars
 * or markup into those surfaces. [A-Za-z0-9_-], length 1..NAME_MAX-1. */
static int name_ok(const char *s)
{
   if (!s || !s[0])
      return 0;
   size_t n = strlen(s);
   if (n >= KB_CURATOR_CUSTOM_NAME_MAX)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      char c = s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

/* "llm"/"index" -> lane; -1 if unrecognised. */
static int lane_of_str(const char *s)
{
   if (!s)
      return -1;
   if (strcmp(s, "llm") == 0)
      return KB_CURATOR_LANE_LLM;
   if (strcmp(s, "index") == 0)
      return KB_CURATOR_LANE_INDEX;
   return -1;
}

size_t kb_curator_custom_stages_parse(const char *json, kb_curator_base_resolver resolve,
                                      kb_curator_custom_t *out, size_t max, int *nrej)
{
   if (nrej)
      *nrej = 0;
   if (!json || !json[0] || !resolve || !out || max == 0)
      return 0;
   if (max > KB_CURATOR_MAX_CUSTOM)
      max = KB_CURATOR_MAX_CUSTOM;
   /* Byte-cap the raw blob before handing it to the parser (A5): bound the work a
    * hostile config can trigger, independent of the config field's own size. */
   if (strlen(json) >= KB_CURATOR_CUSTOM_JSON_MAX)
   {
      if (nrej)
         *nrej = 1; /* signal "input rejected" so the caller WARNs once */
      return 0;
   }

   cJSON *arr = cJSON_Parse(json);
   if (!arr || !cJSON_IsArray(arr))
   {
      if (nrej)
         *nrej = 1;
      if (arr)
         cJSON_Delete(arr);
      return 0;
   }

   size_t k = 0;
   int rej = 0;
   const cJSON *el = NULL;
   cJSON_ArrayForEach(el, arr)
   {
      if (k >= max)
      {
         rej++; /* over the cap: count the rest as rejected, don't silently drop */
         continue;
      }
      if (!cJSON_IsObject(el))
      {
         rej++;
         continue;
      }
      const cJSON *jname = cJSON_GetObjectItemCaseSensitive(el, "name");
      const cJSON *jbase = cJSON_GetObjectItemCaseSensitive(el, "base_op");
      const char *name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : NULL;
      const char *base = (cJSON_IsString(jbase) && jbase->valuestring) ? jbase->valuestring : NULL;
      if (!name_ok(name) || !base || strlen(base) >= KB_CURATOR_CUSTOM_NAME_MAX)
      {
         rej++;
         continue;
      }
      /* Name must not collide with a built-in (resolver hit) or an earlier custom. */
      if (resolve(name) != NULL)
      {
         rej++;
         continue;
      }
      int dup = 0;
      for (size_t j = 0; j < k; j++)
         if (strcmp(out[j].name, name) == 0)
         {
            dup = 1;
            break;
         }
      if (dup)
      {
         rej++;
         continue;
      }
      /* base_op must resolve to a vetted built-in op (injection guard). */
      const kb_curator_stage_desc_t *bd = resolve(base);
      if (!bd || !bd->run)
      {
         rej++;
         continue;
      }
      /* Optional "lane": accept only when it names the base op's native lane.
       * A differing lane is re-laning, which v1 forbids (double-drain hazard). */
      const cJSON *jlane = cJSON_GetObjectItemCaseSensitive(el, "lane");
      if (jlane)
      {
         if (!cJSON_IsString(jlane) || lane_of_str(jlane->valuestring) != (int)bd->lane)
         {
            rej++;
            continue;
         }
      }
      /* Optional "enabled": must be a JSON bool if present; default true. */
      int enabled = 1;
      const cJSON *jen = cJSON_GetObjectItemCaseSensitive(el, "enabled");
      if (jen)
      {
         if (!cJSON_IsBool(jen))
         {
            rej++;
            continue;
         }
         enabled = cJSON_IsTrue(jen) ? 1 : 0;
      }
      /* Optional "budget": default to the base op's; clamp >= 1. */
      int budget = bd->budget > 0 ? bd->budget : 1;
      const cJSON *jbud = cJSON_GetObjectItemCaseSensitive(el, "budget");
      if (jbud && cJSON_IsNumber(jbud))
      {
         budget = (int)jbud->valuedouble;
         if (budget < 1)
            budget = 1;
      }

      kb_curator_custom_t *c = &out[k++];
      memset(c, 0, sizeof(*c));
      snprintf(c->name, sizeof(c->name), "%s", name);
      snprintf(c->base_op, sizeof(c->base_op), "%s", base);
      c->base = bd;
      c->budget = budget;
      c->enabled = enabled;
   }

   cJSON_Delete(arr);
   if (nrej)
      *nrej = rej;
   return k;
}

void kb_curator_custom_to_desc(const kb_curator_custom_t *c, kb_curator_stage_desc_t *desc)
{
   if (!c || !desc)
      return;
   memset(desc, 0, sizeof(*desc));
   desc->name = c->name;
   desc->label = c->name; /* status label = the custom's own name */
   desc->enabled = NULL;  /* presence == enabled; disabled customs aren't composed */
   desc->run = c->base ? c->base->run : NULL;
   desc->budget = c->budget;
   desc->lane = c->base ? c->base->lane : KB_CURATOR_LANE_LLM;
}
