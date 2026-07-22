/* kb_client_tool_registry.c: kb_client wrappers for the tool_registry.*
 * RPC family (lookup, snapshot).  Non-server test builds can short-circuit
 * into in-process DB2 when the sqlite shim is initialized; aimee-server
 * compiles this file with AIMEE_DB2_DISABLED and always uses the RPC path. */

#include "kb_client.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "lifecycle.h"
#include "tool_registry.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in kb_client.c. */
char *kb_v1_action_request(const char *method, cJSON *req);

int kb_client_tool_registry_lookup(const char *name, char *out_input_schema, size_t schema_cap,
                                   char *out_side_effect, size_t se_cap, int *out_enabled,
                                   int *out_found)
{
   if (out_found)
      *out_found = 0;
   if (out_input_schema && schema_cap > 0)
      out_input_schema[0] = '\0';
   if (out_side_effect && se_cap > 0)
      out_side_effect[0] = '\0';
   if (out_enabled)
      *out_enabled = 0;
   if (!name)
      return -1;

#if !defined(AIMEE_DB2_DISABLED)
   if (db2_is_initialized())
   {
      tool_registry_entry_t entry;
      memset(&entry, 0, sizeof(entry));
      int rc = db2_tool_registry_lookup(name, &entry);
      if (rc != 0)
         return -1;
      if (out_found)
         *out_found = entry.found;
      if (entry.found)
      {
         if (out_input_schema && schema_cap > 0)
            snprintf(out_input_schema, schema_cap, "%s", entry.input_schema);
         if (out_side_effect && se_cap > 0)
            snprintf(out_side_effect, se_cap, "%s", entry.side_effect);
         if (out_enabled)
            *out_enabled = entry.enabled;
      }
      return 0;
   }
#endif

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "name", name);
   char *json = kb_v1_action_request("tool_registry.lookup", req);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *found = cJSON_GetObjectItemCaseSensitive(resp, "found");
   if (cJSON_IsBool(found) && cJSON_IsTrue(found))
   {
      if (out_found)
         *out_found = 1;
      cJSON *sch = cJSON_GetObjectItemCaseSensitive(resp, "input_schema");
      cJSON *se = cJSON_GetObjectItemCaseSensitive(resp, "side_effect");
      cJSON *en = cJSON_GetObjectItemCaseSensitive(resp, "enabled");
      if (out_input_schema && cJSON_IsString(sch))
         snprintf(out_input_schema, schema_cap, "%s", sch->valuestring);
      if (out_side_effect && cJSON_IsString(se))
         snprintf(out_side_effect, se_cap, "%s", se->valuestring);
      if (out_enabled && cJSON_IsBool(en))
         *out_enabled = cJSON_IsTrue(en) ? 1 : 0;
   }
   cJSON_Delete(resp);
   return 0;
}

#if !defined(AIMEE_DB2_DISABLED)
struct kbctr_prompts_ctx
{
   cJSON *arr;
};

static int kbctr_collect_prompt(const char *name, const char *prompt, void *user)
{
   struct kbctr_prompts_ctx *ctx = (struct kbctr_prompts_ctx *)user;
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return -1;
   cJSON_AddStringToObject(o, "name", name ? name : "");
   cJSON_AddStringToObject(o, "prompt", prompt ? prompt : "");
   cJSON_AddItemToArray(ctx->arr, o);
   return 0;
}
#endif

char *kb_client_tool_registry_snapshot_json(void)
{
#if !defined(AIMEE_DB2_DISABLED)
   if (db2_is_initialized())
   {
      cJSON *resp = cJSON_CreateObject();
      if (!resp)
         return NULL;
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON *prompts = cJSON_AddArrayToObject(resp, "prompts");
      struct kbctr_prompts_ctx ctx = {prompts};
      db2_tool_registry_iter_prompts(kbctr_collect_prompt, &ctx);
      char *out = cJSON_PrintUnformatted(resp);
      cJSON_Delete(resp);
      return out;
   }
#endif

   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("tool_registry.snapshot", req);
}
