/* plugin.c: plugin manifest parsing and registry persistence */
#include "aimee.h"
#include "headers/plugin.h"
#include "memory_provider.h"
#include "headers/context_engine.h"
#include "cJSON.h"
#include "log.h"
#include "platform_path.h"
#include <dlfcn.h>
#include <dirent.h>

/* Forward declarations */
int plugin_tool_conflicts_with_builtin(const char *tool_name);
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Permission helpers --- */

const char *plugin_permission_name(plugin_permission_t p)
{
   switch (p)
   {
   case PLUGIN_PERM_READ:
      return "read";
   case PLUGIN_PERM_WRITE:
      return "write";
   case PLUGIN_PERM_EXECUTE:
      return "execute";
   case PLUGIN_PERM_DANGEROUS:
      return "dangerous";
   default:
      return "read";
   }
}

plugin_permission_t plugin_permission_from_str(const char *s)
{
   if (!s)
      return PLUGIN_PERM_READ;
   if (strcmp(s, "write") == 0)
      return PLUGIN_PERM_WRITE;
   if (strcmp(s, "execute") == 0)
      return PLUGIN_PERM_EXECUTE;
   if (strcmp(s, "dangerous") == 0)
      return PLUGIN_PERM_DANGEROUS;
   return PLUGIN_PERM_READ;
}

/* --- Registry path --- */

const char *plugin_registry_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;
   snprintf(path, sizeof(path), "%s/" PLUGIN_REGISTRY_FILE, config_default_dir());
   return path;
}

static void ensure_plugins_dir(void)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s/plugins", config_default_dir());
   platform_mkdir_p(dir, 0700);
}

/* --- Serialise / deserialise a plugin_t to/from cJSON --- */

static cJSON *plugin_to_json(const plugin_t *p)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "name", p->name);
   cJSON_AddStringToObject(obj, "version", p->version);
   cJSON_AddStringToObject(obj, "description", p->description);
   cJSON_AddStringToObject(obj, "source_path", p->source_path);
   cJSON_AddStringToObject(obj, "kind", plugin_kind_name(p->kind));
   cJSON_AddBoolToObject(obj, "enabled", p->enabled);
   cJSON_AddBoolToObject(obj, "default_enabled", p->default_enabled);

   /* hooks */
   cJSON *hooks = cJSON_AddArrayToObject(obj, "hooks");
   for (int i = 0; i < p->hook_count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddStringToObject(h, "event", p->hooks[i].event);
      cJSON_AddStringToObject(h, "command", p->hooks[i].command);
      cJSON_AddItemToArray(hooks, h);
   }

   /* tools */
   cJSON *tools = cJSON_AddArrayToObject(obj, "tools");
   for (int i = 0; i < p->tool_count; i++)
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", p->tools[i].name);
      cJSON_AddStringToObject(t, "description", p->tools[i].description);
      cJSON_AddStringToObject(t, "command", p->tools[i].command);
      cJSON_AddStringToObject(t, "permission", plugin_permission_name(p->tools[i].permission));
      cJSON *schema = cJSON_Parse(p->tools[i].input_schema_json);
      if (schema)
         cJSON_AddItemToObject(t, "input_schema", schema);
      cJSON_AddItemToArray(tools, t);
   }

   return obj;
}

static void plugin_from_json(const cJSON *obj, plugin_t *p)
{
   memset(p, 0, sizeof(*p));

   cJSON *v;
#define STR(field, key)                                                                            \
   v = cJSON_GetObjectItemCaseSensitive(obj, key);                                                 \
   if (cJSON_IsString(v))                                                                          \
      snprintf(p->field, sizeof(p->field), "%s", v->valuestring);

   STR(name, "name");
   STR(version, "version");
   STR(description, "description");
   STR(source_path, "source_path");
#undef STR

   v = cJSON_GetObjectItemCaseSensitive(obj, "enabled");
   p->enabled = cJSON_IsTrue(v) ? 1 : 0;
   /* kind */
   v = cJSON_GetObjectItemCaseSensitive(obj, "kind");
   if (cJSON_IsString(v))
      p->kind = plugin_kind_from_str(v->valuestring);

   v = cJSON_GetObjectItemCaseSensitive(obj, "default_enabled");
   p->default_enabled = cJSON_IsTrue(v) ? 1 : 0;

   /* hooks */
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(obj, "hooks");
   cJSON *h = NULL;
   cJSON_ArrayForEach(h, hooks)
   {
      if (p->hook_count >= PLUGIN_MAX_HOOKS)
         break;
      plugin_hook_t *ph = &p->hooks[p->hook_count++];
      cJSON *ev = cJSON_GetObjectItemCaseSensitive(h, "event");
      cJSON *cmd = cJSON_GetObjectItemCaseSensitive(h, "command");
      if (cJSON_IsString(ev))
         snprintf(ph->event, sizeof(ph->event), "%s", ev->valuestring);
      if (cJSON_IsString(cmd))
         snprintf(ph->command, sizeof(ph->command), "%s", cmd->valuestring);
   }

   /* tools */
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(obj, "tools");
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools)
   {
      if (p->tool_count >= PLUGIN_MAX_TOOLS)
         break;
      plugin_tool_t *pt = &p->tools[p->tool_count++];
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
      cJSON *dc = cJSON_GetObjectItemCaseSensitive(t, "description");
      cJSON *cv = cJSON_GetObjectItemCaseSensitive(t, "command");
      cJSON *pm = cJSON_GetObjectItemCaseSensitive(t, "permission");
      cJSON *sch = cJSON_GetObjectItemCaseSensitive(t, "input_schema");
      if (cJSON_IsString(nm))
         snprintf(pt->name, sizeof(pt->name), "%s", nm->valuestring);
      if (cJSON_IsString(dc))
         snprintf(pt->description, sizeof(pt->description), "%s", dc->valuestring);
      if (cJSON_IsString(cv))
         snprintf(pt->command, sizeof(pt->command), "%s", cv->valuestring);
      if (cJSON_IsString(pm))
         pt->permission = plugin_permission_from_str(pm->valuestring);
      if (sch)
      {
         char *s = cJSON_PrintUnformatted(sch);
         if (s)
         {
            snprintf(pt->input_schema_json, sizeof(pt->input_schema_json), "%s", s);
            free(s);
         }
      }
      else
      {
         snprintf(pt->input_schema_json, sizeof(pt->input_schema_json),
                  "{\"type\":\"object\",\"properties\":{}}");
      }
   }
}

/* --- Registry load/save --- */

int plugin_registry_load(plugin_t *plugins, int max)
{
   const char *path = plugin_registry_path();
   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;

   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz <= 0)
   {
      fclose(fp);
      return 0;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return 0;
   }
   size_t nr = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[nr] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return 0;
   }

   int count = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, root)
   {
      if (count >= max)
         break;
      plugin_from_json(item, &plugins[count++]);
   }
   cJSON_Delete(root);
   return count;
}

int plugin_registry_save(const plugin_t *plugins, int count)
{
   ensure_plugins_dir();
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, plugin_to_json(&plugins[i]));

   char *json = cJSON_Print(arr);
   cJSON_Delete(arr);
   if (!json)
      return -1;

   const char *path = plugin_registry_path();
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);

   FILE *fp = fopen(tmp, "w");
   if (!fp)
   {
      free(json);
      return -1;
   }
   fputs(json, fp);
   fclose(fp);
   free(json);

   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

int plugin_registry_get(const char *name, plugin_t *out)
{
   if (!name || !name[0] || !out)
      return -1;

   plugin_t *plugins = calloc((size_t)PLUGIN_MAX_PLUGINS, sizeof(plugin_t));
   if (!plugins)
      return -1;
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);
   for (int i = 0; i < count; i++)
   {
      if (strcmp(plugins[i].name, name) == 0)
      {
         *out = plugins[i];
         free(plugins);
         return 0;
      }
   }
   free(plugins);
   return -1;
}

char *plugin_registry_json(void)
{
   plugin_t *plugins = calloc((size_t)PLUGIN_MAX_PLUGINS, sizeof(plugin_t));
   if (!plugins)
      return strdup("[]");
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      free(plugins);
      return strdup("[]");
   }

   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
         continue;
      cJSON_AddStringToObject(obj, "name", plugins[i].name);
      cJSON_AddStringToObject(obj, "version", plugins[i].version);
      cJSON_AddStringToObject(obj, "kind", plugin_kind_name(plugins[i].kind));
      cJSON_AddBoolToObject(obj, "enabled", plugins[i].enabled);
      cJSON_AddNumberToObject(obj, "hook_count", plugins[i].hook_count);
      cJSON_AddNumberToObject(obj, "tool_count", plugins[i].tool_count);
      cJSON_AddStringToObject(obj, "source_path", plugins[i].source_path);
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   free(plugins);
   return json ? json : strdup("[]");
}

/* --- Manifest parsing --- */

/* Resolve a command path relative to plugin source_path if it starts with ./ */
static void resolve_plugin_path(const char *source_path, const char *rel, char *out, size_t out_len)
{
   if (!rel || !rel[0])
   {
      out[0] = '\0';
      return;
   }
   if (rel[0] == '.' && rel[1] == '/')
      snprintf(out, out_len, "%s/%s", source_path, rel + 2);
   else
      snprintf(out, out_len, "%s", rel);
}

int plugin_manifest_parse(const char *dir, plugin_t *out, char *err_buf, size_t err_len)
{
   char manifest[MAX_PATH_LEN];
   snprintf(manifest, sizeof(manifest), "%s/" PLUGIN_MANIFEST_FILE, dir);

   FILE *fp = fopen(manifest, "r");
   if (!fp)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "cannot open %s: %s", manifest, strerror(errno));
      return -1;
   }
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      if (err_buf)
         snprintf(err_buf, err_len, "out of memory");
      return -1;
   }
   size_t nr = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[nr] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "invalid JSON in %s", manifest);
      return -1;
   }

   memset(out, 0, sizeof(*out));

   cJSON *v;
#define REQUIRE_STR(field, key)                                                                    \
   v = cJSON_GetObjectItemCaseSensitive(root, key);                                                \
   if (!cJSON_IsString(v) || !v->valuestring[0])                                                   \
   {                                                                                               \
      if (err_buf)                                                                                 \
         snprintf(err_buf, err_len, "missing required field '%s' in plugin.json", key);            \
      cJSON_Delete(root);                                                                          \
      return -1;                                                                                   \
   }                                                                                               \
   snprintf(out->field, sizeof(out->field), "%s", v->valuestring);

#define OPT_STR(field, key)                                                                        \
   v = cJSON_GetObjectItemCaseSensitive(root, key);                                                \
   if (cJSON_IsString(v))                                                                          \
      snprintf(out->field, sizeof(out->field), "%s", v->valuestring);

   REQUIRE_STR(name, "name");
   OPT_STR(version, "version");
   OPT_STR(description, "description");
#undef REQUIRE_STR
#undef OPT_STR

   snprintf(out->source_path, sizeof(out->source_path), "%s", dir);
   out->kind = PLUGIN_KIND_STANDALONE;

   v = cJSON_GetObjectItemCaseSensitive(root, "defaultEnabled");
   out->default_enabled = cJSON_IsTrue(v) ? 1 : 0;
   out->enabled = out->default_enabled;

   /* requires_env: ["VAR1", "VAR2"] */
   cJSON *req_env = cJSON_GetObjectItemCaseSensitive(root, "requires_env");
   if (cJSON_IsArray(req_env))
   {
      cJSON *ev = NULL;
      cJSON_ArrayForEach(ev, req_env)
      {
         if (!cJSON_IsString(ev) || !ev->valuestring[0])
            continue;
         if (out->required_env_count >= PLUGIN_MAX_PERMISSIONS)
            break;
         snprintf(out->required_env[out->required_env_count], 64, "%s", ev->valuestring);
         out->required_env_count++;
      }
   }

   /* permissions (informational — stored on the plugin itself) */
   /* hooks: {"PreToolUse": ["./hooks/pre.sh"], "PostToolUse": [...]} */
   cJSON *hooks_obj = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   if (cJSON_IsObject(hooks_obj))
   {
      const char *events[] = {"PreToolUse", "PostToolUse", NULL};
      for (int ei = 0; events[ei]; ei++)
      {
         cJSON *event_arr = cJSON_GetObjectItemCaseSensitive(hooks_obj, events[ei]);
         cJSON *cmd = NULL;
         cJSON_ArrayForEach(cmd, event_arr)
         {
            if (!cJSON_IsString(cmd) || out->hook_count >= PLUGIN_MAX_HOOKS)
               continue;
            plugin_hook_t *ph = &out->hooks[out->hook_count++];
            snprintf(ph->event, sizeof(ph->event), "%s", events[ei]);
            resolve_plugin_path(dir, cmd->valuestring, ph->command, sizeof(ph->command));
         }
      }
   }

   /* tools */
   cJSON *tools_arr = cJSON_GetObjectItemCaseSensitive(root, "tools");
   cJSON *t = NULL;
   cJSON_ArrayForEach(t, tools_arr)
   {
      if (out->tool_count >= PLUGIN_MAX_TOOLS)
         break;
      plugin_tool_t *pt = &out->tools[out->tool_count];

      cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
      cJSON *dc = cJSON_GetObjectItemCaseSensitive(t, "description");
      cJSON *cv = cJSON_GetObjectItemCaseSensitive(t, "command");
      cJSON *pm = cJSON_GetObjectItemCaseSensitive(t, "permission");
      cJSON *sch = cJSON_GetObjectItemCaseSensitive(t, "input_schema");

      if (!cJSON_IsString(nm) || !nm->valuestring[0])
         continue;

      snprintf(pt->name, sizeof(pt->name), "%s", nm->valuestring);
      if (cJSON_IsString(dc))
         snprintf(pt->description, sizeof(pt->description), "%s", dc->valuestring);
      if (cJSON_IsString(cv))
         resolve_plugin_path(dir, cv->valuestring, pt->command, sizeof(pt->command));
      if (cJSON_IsString(pm))
         pt->permission = plugin_permission_from_str(pm->valuestring);

      if (sch)
      {
         char *s = cJSON_PrintUnformatted(sch);
         if (s)
         {
            snprintf(pt->input_schema_json, sizeof(pt->input_schema_json), "%s", s);
            free(s);
         }
      }
      else
      {
         snprintf(pt->input_schema_json, sizeof(pt->input_schema_json),
                  "{\"type\":\"object\",\"properties\":{}}");
      }
      out->tool_count++;
   }

   cJSON_Delete(root);
   return 0;
}

/* --- Install / enable / disable / remove --- */

int plugin_install(const char *source_dir, char *err_buf, size_t err_len)
{
   plugin_t new_plugin;
   if (plugin_manifest_parse(source_dir, &new_plugin, err_buf, err_len) != 0)
      return -1;

   /* Load existing registry */
   plugin_t *plugins = calloc((size_t)PLUGIN_MAX_PLUGINS, sizeof(plugin_t));
   if (!plugins)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "out of memory");
      return -1;
   }
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   /* Check name conflict */
   for (int i = 0; i < count; i++)
   {
      if (strcmp(plugins[i].name, new_plugin.name) == 0)
      {
         if (err_buf)
            snprintf(err_buf, err_len, "plugin '%s' is already installed", new_plugin.name);
         free(plugins);
         return -1;
      }
   }

   /* Check tool conflicts with existing plugins */
   for (int ti = 0; ti < new_plugin.tool_count; ti++)
   {
      /* Check builtin conflict */
      if (plugin_tool_conflicts_with_builtin(new_plugin.tools[ti].name))
      {
         if (err_buf)
            snprintf(err_buf, err_len, "tool '%s' conflicts with a builtin tool name",
                     new_plugin.tools[ti].name);
         free(plugins);
         return -1;
      }
      /* Check conflict with other installed plugins */
      for (int i = 0; i < count; i++)
         for (int tj = 0; tj < plugins[i].tool_count; tj++)
            if (strcmp(plugins[i].tools[tj].name, new_plugin.tools[ti].name) == 0)
            {
               if (err_buf)
                  snprintf(err_buf, err_len, "tool '%s' already declared by plugin '%s'",
                           new_plugin.tools[ti].name, plugins[i].name);
               free(plugins);
               return -1;
            }
   }

   if (count >= PLUGIN_MAX_PLUGINS)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "maximum number of plugins (%d) reached", PLUGIN_MAX_PLUGINS);
      free(plugins);
      return -1;
   }

   plugins[count++] = new_plugin;
   int rc = plugin_registry_save(plugins, count);
   free(plugins);
   return rc;
}

int plugin_set_enabled(const char *name, int enabled, char *err_buf, size_t err_len)
{
   plugin_t *plugins = calloc((size_t)PLUGIN_MAX_PLUGINS, sizeof(plugin_t));
   if (!plugins)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "out of memory");
      return -1;
   }
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   for (int i = 0; i < count; i++)
   {
      if (strcmp(plugins[i].name, name) == 0)
      {
         plugins[i].enabled = enabled;
         int rc = plugin_registry_save(plugins, count);
         free(plugins);
         return rc;
      }
   }

   if (err_buf)
      snprintf(err_buf, err_len, "plugin '%s' not found", name);
   free(plugins);
   return -1;
}

int plugin_remove(const char *name, char *err_buf, size_t err_len)
{
   plugin_t *plugins = calloc((size_t)PLUGIN_MAX_PLUGINS, sizeof(plugin_t));
   if (!plugins)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "out of memory");
      return -1;
   }
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   int found = -1;
   for (int i = 0; i < count; i++)
      if (strcmp(plugins[i].name, name) == 0)
         found = i;

   if (found < 0)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "plugin '%s' not found", name);
      free(plugins);
      return -1;
   }

   /* Shift remaining entries */
   for (int i = found; i < count - 1; i++)
      plugins[i] = plugins[i + 1];
   count--;

   int rc = plugin_registry_save(plugins, count);
   free(plugins);
   return rc;
}

/* --- Project-local plugin discovery --- */

int plugin_discover_local(const char **workspace_roots, int root_count, plugin_t *plugins,
                          int *count, int max)
{
   int added = 0;
   for (int ri = 0; ri < root_count; ri++)
   {
      if (!workspace_roots[ri] || !workspace_roots[ri][0])
         continue;

      char manifest_dir[MAX_PATH_LEN];
      snprintf(manifest_dir, sizeof(manifest_dir), "%s", workspace_roots[ri]);

      char manifest_file[MAX_PATH_LEN];
      snprintf(manifest_file, sizeof(manifest_file), "%s/" PLUGIN_MANIFEST_FILE, manifest_dir);

      if (access(manifest_file, R_OK) != 0)
         continue;

      /* Skip if already registered by source_path */
      int found = 0;
      for (int i = 0; i < *count; i++)
         if (strcmp(plugins[i].source_path, manifest_dir) == 0)
         {
            found = 1;
            break;
         }
      if (found)
         continue;

      if (*count >= max)
         break;

      plugin_t tmp;
      char err[256];
      if (plugin_manifest_parse(manifest_dir, &tmp, err, sizeof(err)) != 0)
      {
         LOG_WARN("plugin", "skipping local plugin in %s: %s", manifest_dir, err);
         continue;
      }
      tmp.kind = PLUGIN_KIND_STANDALONE;
      plugins[(*count)++] = tmp;
      added++;
   }
   return added;
}

/* --- Hook aggregation --- */

int plugin_collect_hooks(const plugin_t *plugins, int count, const char *event, char cmds[][512],
                         int max)
{
   int n = 0;
   for (int i = 0; i < count && n < max; i++)
   {
      if (!plugins[i].enabled)
         continue;
      for (int j = 0; j < plugins[i].hook_count && n < max; j++)
      {
         if (strcmp(plugins[i].hooks[j].event, event) == 0 && plugins[i].hooks[j].command[0])
         {
            snprintf(cmds[n++], 512, "%s", plugins[i].hooks[j].command);
         }
      }
   }
   return n;
}

/* --- Tool access --- */

int plugin_collect_tools(const plugin_t *plugins, int count, plugin_tool_t *out, int max)
{
   int n = 0;
   for (int i = 0; i < count && n < max; i++)
   {
      if (!plugins[i].enabled)
         continue;
      for (int j = 0; j < plugins[i].tool_count && n < max; j++)
         out[n++] = plugins[i].tools[j];
   }
   return n;
}

int plugin_tool_conflicts_with_builtin(const char *tool_name)
{
   /* List of builtin tool names that plugins cannot shadow.
    * This is the set of names registered in mcp_build_tools_list(). */
   static const char *builtins[] = {"get_help",
                                    "search_memory",
                                    "store_memory",
                                    "list_facts",
                                    "memory_briefing",
                                    "memory_alerts",
                                    "memory_recall",
                                    "create_prospective_memory",
                                    "list_prospective_memories",
                                    "complete_prospective_memory",
                                    "create_epistemic_directive",
                                    "list_epistemic_directives",
                                    "resolve_epistemic_directive",
                                    "memory_maintain",
                                    "create_note",
                                    "list_notes",
                                    "search_notes",
                                    "delegate",
                                    "delegate_reply",
                                    "git_status",
                                    "git_diff_summary",
                                    "git_commit",
                                    "git_push",
                                    "git_pull",
                                    "git_fetch",
                                    "git_branch",
                                    "git_log",
                                    "git_issue",
                                    "git_pr",
                                    "git_tag",
                                    "git_reset",
                                    "git_restore",
                                    "git_stash",
                                    "git_clone",
                                    "git_verify",
                                    "run_background_process",
                                    "get_background_output",
                                    "kill_background_process",
                                    "list_background_processes",
                                    "job_start",
                                    "job_status",
                                    "autopilot",
                                    "find_symbol",
                                    "ast_grep_search",
                                    "lsp_definition",
                                    "lsp_references",
                                    "lsp_diagnostics",
                                    "record_attempt",
                                    "list_attempts",
                                    "diagnose_start",
                                    "diagnose_observe",
                                    "diagnose_hypothesize",
                                    "diagnose_evidence",
                                    "diagnose_status",
                                    "rules_list",
                                    "rules_propose",
                                    "learning_propose",
                                    "learning_review",
                                    "list_hosts",
                                    "get_host",
                                    "preview_blast_radius",
                                    "store_workflow",
                                    "search_docs",
                                    "web_search",
                                    NULL};
   for (int i = 0; builtins[i]; i++)
      if (strcmp(tool_name, builtins[i]) == 0)
         return 1;
   return 0;
}

/* --- Kind taxonomy helpers --- */

const char *plugin_kind_name(plugin_kind_t k)
{
   switch (k)
   {
   case PLUGIN_KIND_STANDALONE:
      return "standalone";
   case PLUGIN_KIND_BACKEND:
      return "backend";
   case PLUGIN_KIND_EXCLUSIVE:
      return "exclusive";
   case PLUGIN_KIND_PLATFORM:
      return "platform";
   default:
      return "standalone";
   }
}

plugin_kind_t plugin_kind_from_str(const char *s)
{
   if (!s)
      return PLUGIN_KIND_STANDALONE;
   if (strcmp(s, "backend") == 0)
      return PLUGIN_KIND_BACKEND;
   if (strcmp(s, "exclusive") == 0)
      return PLUGIN_KIND_EXCLUSIVE;
   if (strcmp(s, "platform") == 0)
      return PLUGIN_KIND_PLATFORM;
   return PLUGIN_KIND_STANDALONE;
}

/* --- Global extension registries --- */

delegate_backend_t *plugin_delegate_backends[PLUGIN_MAX_PLUGINS];
int plugin_delegate_backend_count;

plugin_platform_adapter_t plugin_platform_adapters[PLUGIN_MAX_PLUGINS];
int plugin_platform_adapter_count;

plugin_memory_provider_t plugin_memory_providers[PLUGIN_MAX_PLUGINS];
int plugin_memory_provider_count;

plugin_context_engine_t plugin_context_engines[PLUGIN_MAX_PLUGINS];
int plugin_context_engine_count;

plugin_cli_cmd_t plugin_cli_subcommands[PLUGIN_MAX_PLUGINS];
int plugin_cli_subcommand_count;

plugin_slash_cmd_t plugin_slash_commands[PLUGIN_MAX_PLUGINS];
int plugin_slash_command_count;

plugin_model_provider_t plugin_model_providers[PLUGIN_MAX_PLUGINS];
int plugin_model_provider_count;

/* --- plugin_ctx_t lifecycle --- */

static int ctx_register_tool(plugin_ctx_t *ctx, const plugin_tool_t *tool)
{
   (void)ctx;
   (void)tool;
   /* Tool registration goes through plugin_manifest_parse, not ctx.
    * This is for dynamic tool registration at runtime if needed. */
   return 0;
}

static int ctx_register_hook(plugin_ctx_t *ctx, const plugin_hook_t *hook)
{
   (void)ctx;
   (void)hook;
   /* Hook registration goes through plugin_manifest_parse, not ctx.
    * This is for dynamic hook registration at runtime if needed. */
   return 0;
}

static int ctx_register_slash_command(plugin_ctx_t *ctx, const plugin_slash_cmd_t *cmd)
{
   if (!cmd || plugin_slash_command_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_slash_commands[plugin_slash_command_count++] = *cmd;
   return 0;
}

static int ctx_register_cli_subcommand(plugin_ctx_t *ctx, const plugin_cli_cmd_t *cmd)
{
   (void)ctx;
   if (!cmd || plugin_cli_subcommand_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_cli_subcommands[plugin_cli_subcommand_count++] = *cmd;
   return 0;
}

static int ctx_register_delegate_backend(plugin_ctx_t *ctx, delegate_backend_t *backend)
{
   (void)ctx;
   if (!backend || plugin_delegate_backend_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_delegate_backends[plugin_delegate_backend_count++] = backend;
   return 0;
}

static int ctx_register_platform_adapter(plugin_ctx_t *ctx,
                                         const plugin_platform_adapter_t *adapter)
{
   (void)ctx;
   if (!adapter || plugin_platform_adapter_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_platform_adapters[plugin_platform_adapter_count++] = *adapter;
   return 0;
}

static int ctx_register_memory_provider(plugin_ctx_t *ctx, const memory_provider_t *provider)
{
   (void)ctx;
   if (!provider)
      return -1;
   return memory_provider_register(provider);
}

static int ctx_register_context_engine(plugin_ctx_t *ctx, const context_engine_t *engine)
{
   (void)ctx;
   if (!engine)
      return -1;
   return context_engine_register(engine);
}

static int ctx_register_model_provider(plugin_ctx_t *ctx, const plugin_model_provider_t *provider)
{
   (void)ctx;
   if (!provider || plugin_model_provider_count >= PLUGIN_MAX_PLUGINS)
      return -1;
   plugin_model_providers[plugin_model_provider_count++] = *provider;
   return 0;
}

plugin_ctx_t *plugin_ctx_create(void)
{
   plugin_ctx_t *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

   ctx->register_tool = ctx_register_tool;
   ctx->register_hook = ctx_register_hook;
   ctx->register_slash_command = ctx_register_slash_command;
   ctx->register_cli_subcommand = ctx_register_cli_subcommand;
   ctx->register_delegate_backend = ctx_register_delegate_backend;
   ctx->register_platform_adapter = ctx_register_platform_adapter;
   ctx->register_memory_provider = ctx_register_memory_provider;
   ctx->register_context_engine = ctx_register_context_engine;
   ctx->register_model_provider = ctx_register_model_provider;
   /* on_init and on_shutdown are set by the plugin's register() function */

   return ctx;
}

void plugin_ctx_destroy(plugin_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->on_shutdown)
      ctx->on_shutdown(ctx);
   free(ctx);
}

/* --- Collect delegate backends from all plugins --- */

int plugin_collect_delegate_backends(void)
{
   /* plugin_delegate_backends[] is populated during plugin_load_and_register()
    * via ctx_register_delegate_backend(). This function is called after all
    * plugins load to finalize the global registry. */
   return plugin_delegate_backend_count;
}

/* --- Load and register a single plugin --- */

typedef int (*plugin_register_fn)(plugin_ctx_t *ctx);

int plugin_load_and_register(const plugin_t *plugin, char *err_buf, size_t err_len)
{
   if (!plugin || !plugin->source_path[0])
   {
      if (err_buf)
         snprintf(err_buf, err_len, "invalid plugin: no source_path");
      return -1;
   }

   /* Build path to shared library: <source_path>/<name>.so */
   char lib_path[MAX_PATH_LEN];
   snprintf(lib_path, sizeof(lib_path), "%s/%s.so", plugin->source_path, plugin->name);

   /* If no .so exists, skip dynamic loading (plugin provides tools/hooks only) */
   if (access(lib_path, F_OK) != 0)
   {
      /* No dynamic library; parse slash_commands from JSON and register them. */
      char mpath[MAX_PATH_LEN];
      snprintf(mpath, sizeof(mpath), "%s/.aimee-plugin/plugin.json", plugin->source_path);
      FILE *mf = fopen(mpath, "r");
      if (mf)
      {
         fseek(mf, 0, SEEK_END);
         long msz = ftell(mf);
         fseek(mf, 0, SEEK_SET);
         char *mbuf = malloc((size_t)msz + 1);
         if (mbuf)
         {
            size_t mnr = fread(mbuf, 1, (size_t)msz, mf);
            mbuf[mnr] = '\0';
            cJSON *mroot = cJSON_Parse(mbuf);
            free(mbuf);
            if (mroot)
            {
               cJSON *sc_arr = cJSON_GetObjectItemCaseSensitive(mroot, "slash_commands");
               cJSON *sc = NULL;
               cJSON_ArrayForEach(sc, sc_arr)
               {
                  if (plugin_slash_command_count >= PLUGIN_MAX_PLUGINS)
                     break;
                  cJSON *nm = cJSON_GetObjectItemCaseSensitive(sc, "name");
                  cJSON *dc = cJSON_GetObjectItemCaseSensitive(sc, "description");
                  cJSON *cv = cJSON_GetObjectItemCaseSensitive(sc, "command");
                  if (!cJSON_IsString(nm) || !nm->valuestring[0])
                     continue;
                  plugin_slash_cmd_t *cmd = &plugin_slash_commands[plugin_slash_command_count++];
                  snprintf(cmd->name, sizeof(cmd->name), "%s", nm->valuestring);
                  if (cJSON_IsString(dc))
                     snprintf(cmd->description, sizeof(cmd->description), "%s", dc->valuestring);
                  if (cJSON_IsString(cv))
                     resolve_plugin_path(plugin->source_path, cv->valuestring, cmd->command,
                                         sizeof(cmd->command));
               }
               cJSON_Delete(mroot);
            }
         }
         fclose(mf);
      }
      LOG_INFO("plugin", "%s: no .so found, manifest-only mode", plugin->name);
      return 0;
   }

   void *dl = dlopen(lib_path, RTLD_NOW);
   if (!dl)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "dlopen %s failed: %s", lib_path, dlerror());
      LOG_ERROR("plugin", "%s: dlopen failed: %s", plugin->name, dlerror());
      return -1;
   }

   /* Look up the register() entry point */
   plugin_register_fn reg = (plugin_register_fn)dlsym(dl, "register");
   if (!reg)
   {
      /* Try a name-scoped version */
      char sym_name[128];
      snprintf(sym_name, sizeof(sym_name), "plugin_%s_register", plugin->name);
      reg = (plugin_register_fn)dlsym(dl, sym_name);
   }
   if (!reg)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "plugin %s: no register() entry point found", plugin->name);
      dlclose(dl);
      return -1;
   }

   /* Create plugin context */
   plugin_ctx_t *ctx = plugin_ctx_create();
   if (!ctx)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "plugin %s: out of memory", plugin->name);
      dlclose(dl);
      return -1;
   }
   ctx->plugin_name = plugin->name;
   ctx->plugin_version = plugin->version;
   ctx->source_path = plugin->source_path;
   ctx->kind = plugin->kind;

   /* Call the plugin's register() function */
   if (reg(ctx) != 0)
   {
      if (err_buf)
         snprintf(err_buf, err_len, "plugin %s: register() failed", plugin->name);
      plugin_ctx_destroy(ctx);
      dlclose(dl);
      return -1;
   }

   /* Call on_init if set */
   if (ctx->on_init)
   {
      if (ctx->on_init(ctx) != 0)
      {
         if (err_buf)
            snprintf(err_buf, err_len, "plugin %s: on_init() failed", plugin->name);
         plugin_ctx_destroy(ctx);
         dlclose(dl);
         return -1;
      }
   }

   LOG_INFO("plugin",
            "plugin %s registered successfully (kind=%s, backends=%d, adapters=%d, "
            "context_engines=%d, providers=%d)",
            plugin->name, plugin_kind_name(plugin->kind), plugin_delegate_backend_count,
            plugin_platform_adapter_count, plugin_context_engine_count,
            plugin_memory_provider_count);

   /* Don't destroy ctx — on_shutdown is called at process exit */
   /* Don't dlclose — library stays loaded for the lifetime of the process */
   (void)ctx;
   (void)dl;
   return 0;
}

/* --- Load all enabled plugins from the registry --- */

int plugin_load_all_registered(char *err_buf, size_t err_len)
{
   plugin_t plugins[PLUGIN_MAX_PLUGINS];
   int count = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);

   int failures = 0;
   for (int i = 0; i < count; i++)
   {
      if (!plugins[i].enabled)
         continue;
      if (plugin_load_and_register(&plugins[i], err_buf, err_len) != 0)
      {
         LOG_WARN("plugin", "failed to load plugin '%s': %s", plugins[i].name,
                  err_buf ? err_buf : "unknown error");
         failures++;
         if (err_buf)
            err_buf[0] = '\0'; /* clear for next plugin */
      }
   }

   /* Collect delegate backends from all loaded plugins */
   plugin_collect_delegate_backends();

   return failures == 0 ? 0 : -1;
}

/* --- Discover local (project) plugins --- */
