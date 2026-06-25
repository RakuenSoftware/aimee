/* server_mcp_tools.c: server-owned MCP tool discovery */
#include "server.h"
#include "config.h"
#include "db1.h"
#include "mcp_tools.h"
#include "osv_check.h"
#include "toolset.h"
#include "cJSON.h"
#include "log.h"

#include <string.h>

static int server_client_target(const config_mcp_client_t *client, osv_target_t *target)
{
   const char *argv[CONFIG_MCP_MAX_COMMAND_ARGS + 1];
   if (!client || client->transport != CONFIG_MCP_TRANSPORT_STDIO || client->command_count <= 0)
      return -1;
   for (int i = 0; i < client->command_count; i++)
      argv[i] = client->command[i];
   argv[client->command_count] = NULL;
   return osv_infer_target_from_argv(client->command_count, argv, target);
}

static int server_target_allowlisted(const config_t *cfg, const osv_target_t *target)
{
   char key[256];
   snprintf(key, sizeof(key), "%s:%s", target->ecosystem, target->name);
   for (int i = 0; i < cfg->mcp_osv_allow_count; i++)
   {
      if (strcmp(cfg->mcp_osv_allow[i], key) == 0)
         return 1;
   }
   return 0;
}

static const char *server_osv_verdict_name(osv_verdict_t verdict)
{
   switch (verdict)
   {
   case OSV_VERDICT_CLEAN:
      return "clean";
   case OSV_VERDICT_MALWARE:
      return "malware";
   default:
      return "unknown";
   }
}

static cJSON *server_mcp_audit_item(const config_mcp_client_t *client, const osv_target_t *target,
                                    const db1_mcp_osv_cache_row_t *row)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "client", client ? client->name : "");
   cJSON_AddStringToObject(obj, "ecosystem", target ? target->ecosystem : "");
   cJSON_AddStringToObject(obj, "name", target ? target->name : "");
   cJSON_AddStringToObject(obj, "version", target ? target->version : "");
   cJSON_AddStringToObject(obj, "verdict", row ? row->verdict : (target ? "unknown" : "skipped"));
   cJSON_AddStringToObject(obj, "advisory_ids", row ? row->advisory_ids : "");
   cJSON_AddStringToObject(obj, "checked_at", row ? row->checked_at_text : "");
   return obj;
}

/* Server-only tools (not in the shared mcp_build_tools_list): primary-session,
 * persona, and role admin that only the server can service. */
static void append_server_only_tools(cJSON *tools)
{
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "compact_context");
      cJSON_AddStringToObject(t, "description",
                              "Compact primary session history when approaching the context "
                              "limit. Returns before/after counts and an action summary.");
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_AddObjectToObject(s, "properties");
      cJSON *agent = cJSON_AddObjectToObject(props, "agent");
      cJSON_AddStringToObject(agent, "type", "string");
      cJSON_AddStringToObject(agent, "description",
                              "Name of the pool agent to make this session's primary "
                              "(see 'aimee agent list').");
      cJSON *clear = cJSON_AddObjectToObject(props, "clear");
      cJSON_AddStringToObject(clear, "type", "boolean");
      cJSON_AddStringToObject(clear, "description",
                              "If true, clear the pin and revert to the default provider.");
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "set_primary_agent");
      cJSON_AddStringToObject(t, "description",
                              "Pick which configured pool agent acts as this session's primary "
                              "(e.g. MiniMax-M3). Takes effect on the next turn; pass clear:true "
                              "to revert to the default provider.");
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
   {
      /* upsert_persona: create or edit a persona definition. Personas are the
       * source of truth in user config; this writes the on-disk .md so the edit
       * takes effect on the next persona load (for both the primary and any
       * --persona delegate). */
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_AddObjectToObject(s, "properties");
      struct
      {
         const char *key;
         const char *desc;
      } fields[] = {
          {"name", "Persona name (filename-safe: letters, digits, '.', '_', '-'). Required. An "
                   "existing name (built-in or custom) is edited in place."},
          {"description", "One-line summary of the persona."},
          {"persona", "The identity prose (the 'You are ...' body); may contain one %s for the "
                      "working directory."},
          {"principles", "The principles block that leads the persona's prompt."},
          {"brief", "Session-brief hints (Aimee lookup guidance)."},
          {"delegates", "Delegate policy: 'full', 'readonly', or 'none'."},
          {"check_role", "Done-gate delegate role (optional)."},
          {"check_marker", "Verdict marker stem for the done-gate (optional)."},
      };
      for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      {
         cJSON *f = cJSON_AddObjectToObject(props, fields[i].key);
         cJSON_AddStringToObject(f, "type", "string");
         cJSON_AddStringToObject(f, "description", fields[i].desc);
      }
      cJSON *roles = cJSON_AddObjectToObject(props, "roles");
      cJSON_AddStringToObject(roles, "type", "array");
      cJSON_AddStringToObject(roles, "description",
                              "Advertised delegate roles for this persona (array of strings).");
      cJSON *required = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(required, cJSON_CreateString("name"));
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "upsert_persona");
      cJSON_AddStringToObject(
          t, "description",
          "Create or edit a persona (the primary's identity and a --persona delegate's framing). "
          "Writes the persona's config file so the change is the source of truth on the next "
          "load. Pass only 'name' plus the fields to set.");
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
   {
      /* upsert_role_template: create or edit a delegate role template (the body
       * `aimee delegate <role>` uses). Config is the source of truth. */
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *props = cJSON_AddObjectToObject(s, "properties");
      cJSON *role = cJSON_AddObjectToObject(props, "role");
      cJSON_AddStringToObject(role, "type", "string");
      cJSON_AddStringToObject(role, "description",
                              "Role name (filename-safe: letters, digits, '.', '_', '-'). An "
                              "existing role (built-in or custom) is edited in place.");
      cJSON *content = cJSON_AddObjectToObject(props, "content");
      cJSON_AddStringToObject(content, "type", "string");
      cJSON_AddStringToObject(content, "description",
                              "The template body; may contain {{TASK}} and {{CONTEXT}} "
                              "placeholders substituted at delegate launch.");
      cJSON *required = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(required, cJSON_CreateString("role"));
      cJSON_AddItemToArray(required, cJSON_CreateString("content"));
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "upsert_role_template");
      cJSON_AddStringToObject(
          t, "description",
          "Create or edit a delegate role template (the prompt body `aimee delegate <role>` "
          "uses). Writes the role's config file so the change takes effect on the next delegate "
          "run.");
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
}

/* The full served tool surface (shared built-ins + discovery + server-only),
 * BEFORE any presentation-profile filtering. Used by tools/list and by the
 * find_tools/describe_tool discovery handlers so discovery sees every tool. */
cJSON *mcp_build_full_served_list(void)
{
   cJSON *tools = mcp_build_tools_list();
   if (tools)
      append_server_only_tools(tools);
   return tools;
}

int handle_mcp_tools_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "out of memory", NULL);

   cJSON *tools = mcp_build_full_served_list();
   if (!tools)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "out of memory", NULL);
   }

   /* Presentation profile: shrink the initial tools/list for external MCP
    * clients. Default "core" (lean) is lossless — find_tools/describe_tool reach
    * the rest; set AIMEE_MCP_TOOL_PROFILE=full to present everything. Applied at
    * the served-list choke point so mcp_build_tools_list() (and its golden test)
    * stays intact. */
   {
      const char *profile = mcp_tool_profile_effective(NULL);
      int total = cJSON_GetArraySize(tools);
      int removed = mcp_filter_tools_for_profile(tools, NULL);
      if (removed > 0)
         LOG_INFO("mcp-tools", "tools/list profile '%s': presenting %d tools (hid %d)", profile,
                  total - removed, removed);
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "tools", tools);
   return server_send_ok(conn, resp);
}

int handle_mcp_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   config_t cfg;
   config_load(&cfg);
   if (db1_init(cfg.db1_path) != 0)
      return server_send_error(conn, "db1_init failed", cfg.db1_path);

   cJSON *resp = cJSON_CreateObject();
   cJSON *items = cJSON_CreateArray();
   if (!resp || !items)
   {
      cJSON_Delete(resp);
      cJSON_Delete(items);
      return server_send_error(conn, "out of memory", NULL);
   }

   for (int i = 0; i < cfg.mcp_client_count; i++)
   {
      config_mcp_client_t *client = &cfg.mcp_clients[i];
      osv_target_t target;
      if (server_client_target(client, &target) != 0)
      {
         cJSON_AddItemToArray(items, server_mcp_audit_item(client, NULL, NULL));
         continue;
      }
      db1_mcp_osv_cache_row_t row;
      int hit = db1_mcp_osv_cache_get(target.ecosystem, target.name, target.version, 0, &row);
      cJSON_AddItemToArray(items, server_mcp_audit_item(client, &target, hit == 1 ? &row : NULL));
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "items", items);
   return server_send_ok(conn, resp);
}

int handle_mcp_recheck(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   config_t cfg;
   config_load(&cfg);
   if (db1_init(cfg.db1_path) != 0)
      return server_send_error(conn, "db1_init failed", cfg.db1_path);

   cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "name");
   const char *filter = cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : NULL;

   cJSON *resp = cJSON_CreateObject();
   cJSON *items = cJSON_CreateArray();
   if (!resp || !items)
   {
      cJSON_Delete(resp);
      cJSON_Delete(items);
      return server_send_error(conn, "out of memory", NULL);
   }

   int matched = 0;
   for (int i = 0; i < cfg.mcp_client_count; i++)
   {
      config_mcp_client_t *client = &cfg.mcp_clients[i];
      if (filter && strcmp(filter, client->name) != 0)
         continue;
      matched++;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "client", client->name);
      osv_target_t target;
      if (server_client_target(client, &target) != 0)
      {
         cJSON_AddStringToObject(item, "verdict", "skipped");
         cJSON_AddItemToArray(items, item);
         continue;
      }

      osv_result_t result = osv_query_target(cfg.mcp_osv_endpoint, &target, 10000);
      const char *verdict = server_osv_verdict_name(result.verdict);
      if (result.verdict == OSV_VERDICT_MALWARE)
         (void)db1_mcp_osv_cache_upsert(target.ecosystem, target.name, target.version, "malware",
                                        result.advisory_ids);
      else if (result.verdict == OSV_VERDICT_CLEAN)
         (void)db1_mcp_osv_cache_upsert(target.ecosystem, target.name, target.version, "clean", "");
      const char *action = "allow";
      if (result.verdict == OSV_VERDICT_MALWARE)
         action = server_target_allowlisted(&cfg, &target)
                      ? "allow_allowlisted"
                      : (cfg.mcp_osv_enforce ? "block" : "shadow_block");
      (void)db1_mcp_osv_audit(client->name, target.ecosystem, target.name, target.version, verdict,
                              action, result.advisory_ids);
      cJSON_AddStringToObject(item, "ecosystem", target.ecosystem);
      cJSON_AddStringToObject(item, "name", target.name);
      cJSON_AddStringToObject(item, "version", target.version);
      cJSON_AddStringToObject(item, "verdict", verdict);
      cJSON_AddStringToObject(item, "action", action);
      cJSON_AddStringToObject(item, "advisory_ids", result.advisory_ids);
      cJSON_AddItemToArray(items, item);
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "matched", matched > 0);
   cJSON_AddItemToObject(resp, "items", items);
   return server_send_ok(conn, resp);
}

int handle_toolset_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   toolset_registry_t registry;
   char err[TOOLSET_ERROR_MAX] = "";
   if (toolset_registry_load_effective(&registry, err, sizeof(err)) != 0)
      return server_send_error(conn, err[0] ? err : "failed to load toolsets", NULL);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "out of memory", NULL);
   cJSON *sets = cJSON_AddArrayToObject(resp, "toolsets");
   if (!sets)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "out of memory", NULL);
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < registry.count; i++)
   {
      char resolved[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
      int n = toolset_resolve(&registry, registry.sets[i].name, resolved, TOOLSET_MAX_TOOLS, err,
                              sizeof(err));
      cJSON *item = cJSON_CreateObject();
      if (!item)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "out of memory", NULL);
      }
      cJSON_AddStringToObject(item, "name", registry.sets[i].name);
      cJSON_AddNumberToObject(item, "count", n < 0 ? 0 : n);
      cJSON_AddBoolToObject(item, "builtin", registry.sets[i].builtin);
      cJSON_AddItemToArray(sets, item);
   }
   return server_send_ok(conn, resp);
}

int handle_toolset_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(name) || !name->valuestring[0])
      return server_send_error(conn, "name is required", NULL);
   toolset_registry_t registry;
   char err[TOOLSET_ERROR_MAX] = "";
   if (toolset_registry_load_effective(&registry, err, sizeof(err)) != 0)
      return server_send_error(conn, err[0] ? err : "failed to load toolsets", NULL);
   const toolset_def_t *def = toolset_registry_find(&registry, name->valuestring);
   if (!def)
      return server_send_error(conn, "unknown toolset", NULL);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "name", def->name);
   cJSON_AddBoolToObject(resp, "builtin", def->builtin);
   cJSON *includes = cJSON_AddArrayToObject(resp, "include");
   cJSON *tools = cJSON_AddArrayToObject(resp, "tools");
   if (!includes || !tools)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "out of memory", NULL);
   }
   for (int i = 0; i < def->include_count; i++)
      cJSON_AddItemToArray(includes, cJSON_CreateString(def->include[i]));
   for (int i = 0; i < def->tool_count; i++)
      cJSON_AddItemToArray(tools, cJSON_CreateString(def->tools[i]));
   return server_send_ok(conn, resp);
}

int handle_toolset_resolve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const cJSON *name = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(name) || !name->valuestring[0])
      return server_send_error(conn, "name is required", NULL);
   toolset_registry_t registry;
   char err[TOOLSET_ERROR_MAX] = "";
   if (toolset_registry_load_effective(&registry, err, sizeof(err)) != 0)
      return server_send_error(conn, err[0] ? err : "failed to load toolsets", NULL);
   char resolved[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   int n =
       toolset_resolve(&registry, name->valuestring, resolved, TOOLSET_MAX_TOOLS, err, sizeof(err));
   if (n < 0)
      return server_send_error(conn, err[0] ? err : "failed to resolve toolset", NULL);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "name", name->valuestring);
   cJSON *tools = cJSON_AddArrayToObject(resp, "tools");
   if (!tools)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "out of memory", NULL);
   }
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(tools, cJSON_CreateString(resolved[i]));
   return server_send_ok(conn, resp);
}
