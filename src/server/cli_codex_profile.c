/* Typed Codex SDK catalog and effective launch-profile boundary. No process,
 * credentials, durable cache or browser ownership lives in these pure helpers. */
#include "cli_codex_profile.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *codex_string(const cJSON *object, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}
static cJSON *codex_dynamic_definition(const aimee_tool_t *tool)
{
   if (!aimee_ir_tool_valid(tool) || tool->kind == AIMEE_TOOL_HOSTED || tool->cache_control ||
       tool->cache_ttl || tool->strict == AIMEE_STRICT_TRUE || tool->strict == AIMEE_STRICT_NULL)
      return NULL;
   cJSON *out = cJSON_CreateObject();
   const char *kind = tool->kind == AIMEE_TOOL_NAMESPACE ? "namespace" : "function";
   if (!out || !cJSON_AddStringToObject(out, "type", kind) ||
       !cJSON_AddStringToObject(out, "name", tool->name) ||
       !cJSON_AddStringToObject(out, "description", tool->description ? tool->description : ""))
      goto fail;
   if (tool->kind == AIMEE_TOOL_NAMESPACE)
   {
      cJSON *children = cJSON_AddArrayToObject(out, "tools");
      if (!children)
         goto fail;
      for (int i = 0; i < tool->n_children; i++)
      {
         cJSON *child = codex_dynamic_definition(tool->children + i);
         if (!child || !cJSON_AddItemToArray(children, child))
         {
            cJSON_Delete(child);
            goto fail;
         }
      }
   }
   else
   {
      cJSON *schema = cJSON_Duplicate(tool->schema, 1);
      if (!schema || !cJSON_AddItemToObject(out, "inputSchema", schema))
      {
         cJSON_Delete(schema);
         goto fail;
      }
      if (tool->has_defer_loading &&
          !cJSON_AddBoolToObject(out, "deferLoading", tool->defer_loading))
         goto fail;
   }
   return out;
fail:
   cJSON_Delete(out);
   return NULL;
}
cJSON *cli_codex_dynamic_tools(const aimee_request_t *catalog)
{
   if (!catalog || aimee_ir_request_admit(catalog))
      return NULL;
   cJSON *out = cJSON_CreateArray();
   if (!out)
      return NULL;
   for (int i = 0; i < catalog->n_tools; i++)
   {
      cJSON *entry = codex_dynamic_definition(catalog->tools + i);
      if (!entry || !cJSON_AddItemToArray(out, entry))
      {
         cJSON_Delete(entry);
         cJSON_Delete(out);
         return NULL;
      }
   }
   return out;
}
int cli_codex_dynamic_call(const cJSON *params, const aimee_request_t *catalog, aimee_block_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!catalog || !cJSON_IsObject(params) || !aimee_ir_json_unique(params) ||
       aimee_ir_request_admit(catalog))
      return -1;
   const char *name = codex_string(params, "tool"), *id = codex_string(params, "callId");
   const cJSON *ns = cJSON_GetObjectItemCaseSensitive(params, "namespace");
   const char *space = cJSON_GetStringValue(ns);
   const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
   if (!name || !name[0] || !id || !id[0] || !cJSON_IsObject(arguments) ||
       (ns && !cJSON_IsNull(ns) && (!space || !space[0])))
      return -1;
   int matches = 0;
   for (int i = 0; i < catalog->n_tools; i++)
   {
      const aimee_tool_t *tool = catalog->tools + i;
      if (!space && tool->kind == AIMEE_TOOL_FUNCTION && !strcmp(tool->name, name))
         matches++;
      if (space && tool->kind == AIMEE_TOOL_NAMESPACE && !strcmp(tool->name, space))
         for (int j = 0; j < tool->n_children; j++)
            if (tool->children[j].kind == AIMEE_TOOL_FUNCTION &&
                !strcmp(tool->children[j].name, name))
               matches++;
   }
   if (matches != 1)
      return -1;
   *out = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                          .tool_id = strdup(id),
                          .tool_name = strdup(name),
                          .tool_namespace = space ? strdup(space) : NULL,
                          .tool_input = cJSON_Duplicate(arguments, 1)};
   aimee_message_t message = {.role = "assistant", .blocks = out, .n_blocks = 1};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   if (!out->tool_id || !out->tool_name || (space && !out->tool_namespace) || !out->tool_input ||
       aimee_ir_request_admit(&request))
   {
      aimee_block_free_contents(out);
      memset(out, 0, sizeof *out);
      return -1;
   }
   return 0;
}
cJSON *cli_codex_dynamic_result(const aimee_block_t *result)
{
   if (!result || result->type != AIMEE_BLK_TOOL_RESULT || !cJSON_IsString(result->tool_result))
      return NULL;
   aimee_message_t message = {.role = "user", .blocks = (aimee_block_t *)result, .n_blocks = 1};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   if (aimee_ir_request_admit(&request))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   cJSON *items = out ? cJSON_AddArrayToObject(out, "contentItems") : NULL;
   cJSON *text = items ? cJSON_CreateObject() : NULL;
   if (!text || !cJSON_AddItemToArray(items, text))
   {
      cJSON_Delete(text);
      cJSON_Delete(out);
      return NULL;
   }
   if (!cJSON_AddStringToObject(text, "type", "inputText") ||
       !cJSON_AddStringToObject(text, "text", result->tool_result->valuestring) ||
       !cJSON_AddBoolToObject(out, "success", !result->tool_is_error))
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}

/* Only an owned launch can consume a native cache. Verify the effective SDK
 * configuration and its contributing layers before creating/resuming a thread. */
static int codex_config_contains(const cJSON *actual, const cJSON *expected)
{
   if (!cJSON_IsObject(actual) || !cJSON_IsObject(expected))
      return 0;
   const cJSON *item;
   cJSON_ArrayForEach(item, expected)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(actual, item->string);
      if (cJSON_IsObject(item) ? !codex_config_contains(value, item)
                               : !value || !cJSON_Compare(value, item, 1))
         return 0;
   }
   return 1;
}
static int codex_config_private_paths(cJSON *value, const char *home)
{
   if (cJSON_IsString(value))
   {
      size_t n = strlen(home);
      if (!strncmp(value->valuestring, home, n) &&
          (!value->valuestring[n] || value->valuestring[n] == '/'))
      {
         size_t length = strlen(value->valuestring + n) + 32;
         char *normalized = malloc(length);
         if (!normalized)
            return -1;
         snprintf(normalized, length, "$NATIVE_HOME%s", value->valuestring + n);
         int ok = cJSON_SetValuestring(value, normalized) != NULL;
         free(normalized);
         return ok ? 0 : -1;
      }
   }
   cJSON *item;
   cJSON_ArrayForEach(item, value) if (codex_config_private_paths(item, home)) return -1;
   return 0;
}
int cli_codex_profile_verify(const cli_codex_host_launch_t *host, const cJSON *reply,
                             char out_digest[65])
{
   if (!host || !host->home || !host->cwd || !host->gateway_url || !host->gateway_token ||
       !host->expected_config || !out_digest)
      return -1;
   const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
   const cJSON *config = cJSON_GetObjectItemCaseSensitive(result, "config");
   const cJSON *layers = cJSON_GetObjectItemCaseSensitive(result, "layers");
   int valid = reply && !cJSON_GetObjectItemCaseSensitive(reply, "error") &&
               codex_config_contains(config, host->expected_config) && cJSON_IsArray(layers);
   int user_seen = 0;
   const cJSON *layer;
   cJSON_ArrayForEach(layer, layers)
   {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(layer, "name");
      const cJSON *settings = cJSON_GetObjectItemCaseSensitive(layer, "config");
      const char *kind = codex_string(name, "type");
      if (kind && !strcmp(kind, "project"))
      {
         const char *disabled = codex_string(layer, "disabledReason");
         valid &= disabled && disabled[0];
      }
      else if (kind && !strcmp(kind, "user"))
      {
         char file[MAX_PATH_LEN];
         const char *path = codex_string(name, "file");
         valid &=
             snprintf(file, sizeof file, "%s/.codex/config.toml", host->home) < (int)sizeof file &&
             path && !strcmp(path, file) && !user_seen++ &&
             cJSON_Compare(settings, host->expected_config, 1);
      }
      else
         valid &= cJSON_IsObject(settings) && !cJSON_GetArraySize(settings);
   }
   valid &= user_seen == 1;
   const cJSON *mcp = cJSON_GetObjectItemCaseSensitive(config, "mcp_servers");
   const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(config, "plugins");
   const cJSON *notify = cJSON_GetObjectItemCaseSensitive(config, "notify");
   valid &=
       (!mcp || cJSON_IsNull(mcp) || (cJSON_IsObject(mcp) && !cJSON_GetArraySize(mcp))) &&
       (!plugins || cJSON_IsNull(plugins) ||
        (cJSON_IsObject(plugins) && !cJSON_GetArraySize(plugins))) &&
       (!notify || cJSON_IsNull(notify) || (cJSON_IsArray(notify) && !cJSON_GetArraySize(notify)));
   cJSON *owned = valid ? cJSON_Duplicate(config, 1) : NULL;
   if (!owned)
      return -1;
   cJSON *provider = cJSON_GetObjectItemCaseSensitive(
       cJSON_GetObjectItemCaseSensitive(owned, "model_providers"), "aimee_ir");
   cJSON *headers = cJSON_GetObjectItemCaseSensitive(provider, "http_headers");
   char authorization[320], url[320];
   valid = snprintf(authorization, sizeof authorization, "Bearer %s", host->gateway_token) <
               (int)sizeof authorization &&
           snprintf(url, sizeof url, "%s/v1", host->gateway_url) < (int)sizeof url &&
           codex_string(provider, "base_url") && !strcmp(codex_string(provider, "base_url"), url) &&
           codex_string(headers, "Authorization") &&
           !strcmp(codex_string(headers, "Authorization"), authorization);
   /* Ephemeral transport credentials and private HOME names are not cache
    * identity. Normalize only after proving they are this owner's resources. */
   valid = valid &&
           cJSON_ReplaceItemInObjectCaseSensitive(provider, "base_url",
                                                  cJSON_CreateString("$OWNED_GATEWAY/v1")) &&
           cJSON_ReplaceItemInObjectCaseSensitive(headers, "Authorization",
                                                  cJSON_CreateString("$OWNED_CAPABILITY")) &&
           !codex_config_private_paths(owned, host->home);
   if (valid)
   {
      cJSON *projects = cJSON_GetObjectItemCaseSensitive(owned, "projects");
      cJSON *project = cJSON_DetachItemFromObjectCaseSensitive(projects, host->cwd);
      if (!project || !cJSON_AddItemToObject(projects, "$NATIVE_WORKSPACE", project))
      {
         cJSON_Delete(project);
         valid = 0;
      }
   }
   cJSON *canonical = valid ? aimee_ir_json_canonical_copy(owned) : NULL;
   char *wire = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
   cJSON_Delete(owned);
   cJSON_Delete(canonical);
   char digest[65];
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), digest) : -1;
   free(wire);
   if (rc)
      return -1;
   memcpy(out_digest, digest, 65);
   return 0;
}
