/* cmd_plugin.c: aimee plugin install/list/enable/disable/remove CLI commands */
#include "aimee.h"
#include "aimee/plugin-loader/plugin.h"
#include "commands.h"
#include "cJSON.h"
#include <errno.h>
#include <unistd.h>

/* --- install --- */

static void plugin_subcmd_install(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee plugin install <path>");

   const char *source = argv[0];

   /* Resolve to absolute path */
   char abs_path[MAX_PATH_LEN];
   if (realpath(source, abs_path) == NULL)
      fatal("plugin install: cannot resolve path '%s': %s", source, strerror(errno));

   char err[512];
   if (plugin_install(abs_path, err, sizeof(err)) != 0)
      fatal("plugin install: %s", err);

   /* Parse manifest to get the name */
   plugin_t p;
   plugin_manifest_parse(abs_path, &p, NULL, 0);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "name", p.name);
      cJSON_AddStringToObject(obj, "version", p.version);
      cJSON_AddStringToObject(obj, "source", abs_path);
      cJSON_AddStringToObject(obj, "status", "installed");
      char *js = cJSON_PrintUnformatted(obj);
      printf("%s\n", js);
      free(js);
      cJSON_Delete(obj);
   }
   else
   {
      printf("Installed plugin '%s' %s from %s\n", p.name, p.version, abs_path);
   }
}

/* --- list --- */

static void plugin_subcmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   plugin_t plugins[PLUGIN_MAX_PLUGINS];
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   /* Also load project-local plugins from configured workspaces */
   config_t cfg;
   config_load(&cfg);
   const char *roots[64];
   for (int i = 0; i < cfg.workspace_count && i < 64; i++)
      roots[i] = cfg.workspaces[i];
   plugin_discover_local(roots, cfg.workspace_count, plugins, &count, PLUGIN_MAX_PLUGINS);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", plugins[i].name);
         cJSON_AddStringToObject(obj, "version", plugins[i].version);
         cJSON_AddStringToObject(obj, "kind", plugin_kind_name(plugins[i].kind));
         cJSON_AddBoolToObject(obj, "enabled", plugins[i].enabled);
         cJSON_AddStringToObject(obj, "source", plugins[i].source_path);
         cJSON_AddNumberToObject(obj, "hooks", plugins[i].hook_count);
         cJSON_AddNumberToObject(obj, "tools", plugins[i].tool_count);
         cJSON_AddItemToArray(arr, obj);
      }
      char *js = cJSON_PrintUnformatted(arr);
      printf("%s\n", js);
      free(js);
      cJSON_Delete(arr);
   }
   else if (count == 0)
   {
      printf("No plugins installed.\n");
   }
   else
   {
      printf("%-24s %-10s %-8s %-8s %s\n", "NAME", "VERSION", "KIND", "STATUS", "SOURCE");
      for (int i = 0; i < count; i++)
      {
         printf("%-24s %-10s %-8s %-8s %s\n", plugins[i].name,
                plugins[i].version[0] ? plugins[i].version : "-", plugin_kind_name(plugins[i].kind),
                plugins[i].enabled ? "enabled" : "disabled", plugins[i].source_path);
      }
   }
}

/* --- enable --- */

static void plugin_subcmd_enable(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
      fatal("usage: aimee plugin enable <name>");

   char err[512];
   if (plugin_set_enabled(argv[0], 1, err, sizeof(err)) != 0)
      fatal("plugin enable: %s", err);

   printf("Plugin '%s' enabled.\n", argv[0]);
}

/* --- disable --- */

static void plugin_subcmd_disable(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
      fatal("usage: aimee plugin disable <name>");

   char err[512];
   if (plugin_set_enabled(argv[0], 0, err, sizeof(err)) != 0)
      fatal("plugin disable: %s", err);

   printf("Plugin '%s' disabled.\n", argv[0]);
}

/* --- remove --- */

static void plugin_subcmd_remove(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
      fatal("usage: aimee plugin remove <name>");

   char err[512];
   if (plugin_remove(argv[0], err, sizeof(err)) != 0)
      fatal("plugin remove: %s", err);

   printf("Plugin '%s' removed.\n", argv[0]);
}

/* --- subcommand table --- */

static const subcmd_t plugin_subcmds[] = {
    {"install", "Install a plugin from a local directory", plugin_subcmd_install},
    {"list", "List installed plugins", plugin_subcmd_list},
    {"enable", "Enable a plugin by name", plugin_subcmd_enable},
    {"disable", "Disable a plugin by name", plugin_subcmd_disable},
    {"remove", "Remove a plugin by name", plugin_subcmd_remove},
    {NULL, NULL, NULL},
};

void cmd_plugin(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("plugin", plugin_subcmds);
      return;
   }
   subcmd_dispatch(plugin_subcmds, argv[0], ctx, argc - 1, argv + 1);
}
