/* server_http_surfaces.c: the agent-surface projection served by
 * GET /v1/capabilities.
 *
 * Split out of server_http.c to keep that translation unit under the 2500-line
 * build-integrity limit, and because this is one self-contained thing: take the
 * command registry's own view of which capabilities are CLI- or MCP-reachable,
 * and fold in whatever the kb contributes for its own surfaces. */
#include "server_http.h"
#include "server_http_internal.h"
#include "command_registry.h"
#include "cJSON.h"
#include "aimee.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static server_http_json_provider g_kb_agent_surfaces_provider;

void server_http_set_kb_agent_surfaces_provider(server_http_json_provider fn)
{
   g_kb_agent_surfaces_provider = fn;
}

static int string_array_contains(const cJSON *array, const char *value)
{
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, array) if (cJSON_IsString(item) &&
                                       strcmp(item->valuestring, value) == 0) return 1;
   return 0;
}

static void merge_surface_array(cJSON *projection, const cJSON *additional, const char *name)
{
   cJSON *dst = cJSON_GetObjectItemCaseSensitive(projection, name);
   const cJSON *src = cJSON_GetObjectItemCaseSensitive(additional, name);
   if (!cJSON_IsArray(dst) || !cJSON_IsArray(src))
      return;
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, src) if (cJSON_IsString(item) && item->valuestring[0] &&
                                     !string_array_contains(dst, item->valuestring))
       cJSON_AddItemToArray(dst, cJSON_CreateString(item->valuestring));
}

static void merge_kb_agent_surfaces(cJSON *projection)
{
   if (!g_kb_agent_surfaces_provider)
      return;
   char *json = g_kb_agent_surfaces_provider();
   if (!json)
      return;
   cJSON *additional = cJSON_Parse(json);
   free(json);
   if (cJSON_IsObject(additional))
   {
      merge_surface_array(projection, additional, "cli_only");
      merge_surface_array(projection, additional, "mcp_only");
   }
   cJSON_Delete(additional);
}

int route_capabilities(char *resp, int cap)
{
   /* The resources this HTTP surface currently serves; grows with the API. */
   static const char *const names[] = {
       "personas", "sessions",  "models", "chat",    "embeddings", "responses",
       "rules",    "kb",        "memory", "notes",   "dashboard",  "agents",
       "roadmap",  "curiosity", "runs",   "openapi", NULL};
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return 500;
   cJSON *capabilities = cJSON_AddArrayToObject(o, "capabilities");
   for (int i = 0; names[i]; i++)
      cJSON_AddItemToArray(capabilities, cJSON_CreateString(names[i]));
   /* Which surfaces each capability is reachable on. A client that prefers the
    * CLI needs to know what is CLI-reachable BEFORE it registers, rather than
    * discovering it by calling and failing. The kb contributes its own
    * projection when it has one. */
   cJSON *surfaces = aimee_command_agent_surfaces_json();
   if (surfaces)
      merge_kb_agent_surfaces(surfaces);
   cJSON_AddItemToObject(o, "agent_surfaces", surfaces ? surfaces : cJSON_CreateObject());
   cJSON_AddStringToObject(o, "version", AIMEE_VERSION);
   cJSON_AddStringToObject(o, "service", "aimee-server");
   char *json = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   if (!json)
      return 500;
   snprintf(resp, (size_t)cap, "%s", json);
   free(json);
   return 200;
}
