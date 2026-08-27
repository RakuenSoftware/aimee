/* kb_client_tool_registry.c: kb_client wrappers for the tool_registry.*
 * RPC family (lookup, snapshot).  Non-server test builds can short-circuit
 * into in-process DB2 when the sqlite shim is initialized; aimee-server
 * compiles this file with AIMEE_DB2_DISABLED and always uses the RPC path. */

#include "kb_client.h"
#include "cJSON.h"
#include "lifecycle.h"
#include "tool_registry.h"

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

char *kb_client_tool_registry_snapshot_json(void)
{
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

   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("tool_registry.snapshot", req);
}

/* Invoke a tool on an MCP plugin the KB hosts (config install: kb), over the
 * mTLS /v1/actions/mcp.call channel. Always an RPC — plugins run only in the
 * hosting daemon, never in-process here, so there is no DB2 short-circuit.
 * On success returns 0 and, if out_result is non-NULL, sets it to an owned cJSON
 * (the plugin's tools/call result). On failure returns -1 with a message in
 * err_buf. |args| is borrowed (deep-copied into the request). */
int kb_client_mcp_call(const char *qualified_name, const cJSON *args, int timeout_ms,
                       const char *actor, cJSON **out_result, char *err_buf, size_t err_buf_len)
{
   if (out_result)
      *out_result = NULL;
   if (err_buf && err_buf_len)
      err_buf[0] = '\0';
   if (!qualified_name || !qualified_name[0])
   {
      if (err_buf && err_buf_len)
         snprintf(err_buf, err_buf_len, "missing tool name");
      return -1;
   }

   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "name", qualified_name);
   if (args)
   {
      cJSON *args_copy = cJSON_Duplicate(args, 1);
      if (args_copy)
         cJSON_AddItemToObject(req, "arguments", args_copy);
   }
   if (timeout_ms > 0)
      cJSON_AddNumberToObject(req, "timeout_ms", timeout_ms);
   if (actor && actor[0])
      cJSON_AddStringToObject(req, "actor", actor); /* kb records it as the audit actor */

   char *json = kb_v1_action_request("mcp.call", req); /* consumes req */
   if (!json)
   {
      if (err_buf && err_buf_len)
         snprintf(err_buf, err_buf_len, "mcp.call transport failed");
      return -1;
   }
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
   {
      if (err_buf && err_buf_len)
         snprintf(err_buf, err_buf_len, "mcp.call returned an invalid response");
      return -1;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *e = cJSON_GetObjectItemCaseSensitive(resp, "error");
      if (err_buf && err_buf_len)
         snprintf(err_buf, err_buf_len, "%s",
                  cJSON_IsString(e) ? e->valuestring : "mcp.call failed");
      cJSON_Delete(resp);
      return -1;
   }

   cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(resp, "result");
   cJSON_Delete(resp);
   if (out_result)
      *out_result = result;
   else
      cJSON_Delete(result);
   return 0;
}
