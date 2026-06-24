/* gateway_pipeline.c: the gateway's per-call memory stage. See gateway_pipeline.h.
 * Server layer: depends on ingress_preinject (the gated envelope builder) + cJSON.
 * No new behaviour — this is the single home for the context pre-injection that
 * previously lived inline at each ingress handler. */
#include "gateway_pipeline.h"
#include "ingress_preinject.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

char *gateway_pipeline_memory_envelope(const char *query)
{
   return ingress_preinject_build(query, 0);
}

void gateway_pipeline_memory_apply_messages(cJSON *req)
{
   if (!req)
      return;
   char *query =
       ingress_preinject_query_from_messages(cJSON_GetObjectItemCaseSensitive(req, "messages"));
   if (!query)
      return;
   char *env = ingress_preinject_build(query, 0);
   free(query);
   if (!env)
      return;

   cJSON *sys = cJSON_GetObjectItemCaseSensitive(req, "system");
   if (cJSON_IsArray(sys))
   {
      /* Append as a trailing system text block so a cache_control'd system prefix
       * (Claude Code sends cached system blocks) stays stable and caching hits. */
      cJSON *blk = cJSON_CreateObject();
      if (blk)
      {
         cJSON_AddStringToObject(blk, "type", "text");
         cJSON_AddStringToObject(blk, "text", env);
         cJSON_AddItemToArray(sys, blk);
      }
   }
   else if (cJSON_IsString(sys) && sys->valuestring && sys->valuestring[0])
   {
      size_t n = strlen(sys->valuestring) + 2 + strlen(env) + 1;
      char *joined = malloc(n);
      if (joined)
      {
         snprintf(joined, n, "%s\n\n%s", sys->valuestring, env);
         cJSON_ReplaceItemInObjectCaseSensitive(req, "system", cJSON_CreateString(joined));
         free(joined);
      }
   }
   else
   {
      /* system absent or empty: the envelope becomes the system prompt. */
      cJSON_DeleteItemFromObjectCaseSensitive(req, "system");
      cJSON_AddStringToObject(req, "system", env);
   }
   free(env);
}

void gateway_pipeline_memory_apply_instructions(char **instructions, const cJSON *messages)
{
   if (!instructions)
      return;
   char *query = ingress_preinject_query_from_messages(messages);
   char *env = ingress_preinject_build(query, 0);
   free(query);
   if (!env)
      return;
   char *merged = ingress_preinject_apply(*instructions, env);
   if (merged)
   {
      free(*instructions);
      *instructions = merged;
   }
   free(env);
}
