/* cmd_mcp.c: MCP operator commands. */
#include "commands.h"
#include "config.h"
#include "db1_client/db1.h"
#include "osv_check.h"
#include "render.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static const char *verdict_name(osv_verdict_t verdict)
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

static int target_allowlisted(const osv_target_t *target)
{
   char key[256];
   snprintf(key, sizeof(key), "%s:%s", target->ecosystem, target->name);
   for (int i = 0; i < config_mcp_osv_allow_count(); i++)
   {
      if (strcmp(config_mcp_osv_allow(i), key) == 0)
         return 1;
   }
   return 0;
}

static int client_target(const config_mcp_client_t *client, osv_target_t *target)
{
   const char *argv[CONFIG_MCP_MAX_COMMAND_ARGS + 1];
   if (!client || client->transport != CONFIG_MCP_TRANSPORT_STDIO || client->command_count <= 0)
      return -1;
   for (int i = 0; i < client->command_count; i++)
      argv[i] = client->command[i];
   argv[client->command_count] = NULL;
   return osv_infer_target_from_argv(client->command_count, argv, target);
}

static cJSON *audit_row_json(const config_mcp_client_t *client, const osv_target_t *target,
                             const db1_mcp_osv_cache_row_t *row)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "client", client->name);
   cJSON_AddStringToObject(obj, "ecosystem", target ? target->ecosystem : "");
   cJSON_AddStringToObject(obj, "name", target ? target->name : "");
   cJSON_AddStringToObject(obj, "version", target ? target->version : "");
   if (row)
   {
      cJSON_AddStringToObject(obj, "verdict", row->verdict);
      cJSON_AddStringToObject(obj, "advisory_ids", row->advisory_ids);
      cJSON_AddStringToObject(obj, "checked_at", row->checked_at_text);
   }
   else
   {
      cJSON_AddStringToObject(obj, "verdict", target ? "unknown" : "skipped");
      cJSON_AddStringToObject(obj, "advisory_ids", "");
      cJSON_AddStringToObject(obj, "checked_at", "");
   }
   return obj;
}

static void mcp_cmd_audit(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   if (!db1_store_ready())
   {
      fprintf(stderr, "mcp audit: db1_init failed for %s\n", config_db1_path());
      return;
   }

   cJSON *arr = ctx && ctx->json_output ? cJSON_CreateArray() : NULL;
   if (!arr)
      printf("%-20s %-8s %-36s %-12s %-20s %s\n", "client", "eco", "package", "verdict",
             "checked_at", "advisories");

   for (int i = 0; i < config_mcp_client_count(); i++)
   {
      config_mcp_client_t client_buf;
      if (config_mcp_client_at(i, &client_buf) != 0)
         continue;
      config_mcp_client_t *client = &client_buf;
      osv_target_t target;
      if (client_target(client, &target) != 0)
      {
         if (arr)
            cJSON_AddItemToArray(arr, audit_row_json(client, NULL, NULL));
         else
            printf("%-20s %-8s %-36s %-12s %-20s %s\n", client->name, "-", "-", "skipped", "", "");
         continue;
      }

      db1_mcp_osv_cache_row_t row;
      int hit = db1_mcp_osv_cache_get(target.ecosystem, target.name, target.version, 0, &row);
      if (arr)
         cJSON_AddItemToArray(arr, audit_row_json(client, &target, hit == 1 ? &row : NULL));
      else
         printf("%-20s %-8s %-36s %-12s %-20s %s\n", client->name, target.ecosystem, target.name,
                hit == 1 ? row.verdict : "unknown", hit == 1 ? row.checked_at_text : "",
                hit == 1 ? row.advisory_ids : "");
   }

   if (arr)
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
}

static void mcp_cmd_recheck(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   const char *filter = argc >= 1 ? argv[0] : NULL;
   if (!db1_store_ready())
   {
      fprintf(stderr, "mcp recheck: db1_init failed for %s\n", config_db1_path());
      return;
   }

   int checked = 0;
   for (int i = 0; i < config_mcp_client_count(); i++)
   {
      config_mcp_client_t client_buf;
      if (config_mcp_client_at(i, &client_buf) != 0)
         continue;
      config_mcp_client_t *client = &client_buf;
      if (filter && strcmp(filter, client->name) != 0)
         continue;
      osv_target_t target;
      if (client_target(client, &target) != 0)
      {
         printf("%s: skipped (no package-manager target)\n", client->name);
         continue;
      }
      osv_result_t result = osv_query_target(config_mcp_osv_endpoint(), &target, 10000);
      const char *verdict = verdict_name(result.verdict);
      if (result.verdict == OSV_VERDICT_MALWARE)
         (void)db1_mcp_osv_cache_upsert(target.ecosystem, target.name, target.version, "malware",
                                        result.advisory_ids);
      else if (result.verdict == OSV_VERDICT_CLEAN)
         (void)db1_mcp_osv_cache_upsert(target.ecosystem, target.name, target.version, "clean", "");
      const char *action = "allow";
      if (result.verdict == OSV_VERDICT_MALWARE)
         action = target_allowlisted(&target)
                      ? "allow_allowlisted"
                      : (config_mcp_osv_enforce() ? "block" : "shadow_block");
      (void)db1_mcp_osv_audit(client->name, target.ecosystem, target.name, target.version, verdict,
                              action, result.advisory_ids);
      printf("%s: %s:%s %s%s%s\n", client->name, target.ecosystem, target.name, verdict,
             result.advisory_ids[0] ? " " : "", result.advisory_ids);
      checked++;
   }
   if (filter && checked == 0)
      fprintf(stderr, "mcp recheck: no MCP client named %s\n", filter);
}

static const subcmd_t mcp_subcmds[] = {
    {"audit", "List registered MCP servers with last OSV verdict", mcp_cmd_audit},
    {"recheck", "Force a fresh OSV query for all clients or one client name", mcp_cmd_recheck},
    {NULL, NULL, NULL},
};

const subcmd_t *get_mcp_subcmds(void)
{
   return mcp_subcmds;
}

void cmd_mcp(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1 || strcmp(argv[0], "help") == 0 || strcmp(argv[0], "--help") == 0)
   {
      subcmd_usage("mcp", mcp_subcmds);
      return;
   }
   if (subcmd_dispatch(mcp_subcmds, argv[0], ctx, argc - 1, argv + 1) != 0)
      fprintf(stderr, "mcp: unknown subcommand '%s'\n", argv[0]);
}
