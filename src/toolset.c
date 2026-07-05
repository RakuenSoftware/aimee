#include "toolset.h"

#include "aimee.h"
#include "aimee_home.h"
#include "log.h"
#include "yaml.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>

typedef struct
{
   const char *name;
   const char *include[TOOLSET_MAX_INCLUDE];
   const char *tools[TOOLSET_MAX_TOOLS];
} builtin_toolset_t;

static const builtin_toolset_t BUILTINS[] = {
    {"core",
     {NULL},
     {"read_file", "list_files", "grep", "code_search", "find_symbol", "search_memory", NULL}},
    {"git", {NULL}, {"git_status", "git_log", "git_diff", NULL}},
    {"web", {NULL}, {"web_search", NULL}},
    {"readonly", {"core", "git", NULL}, {"verify", "env_get", "test", NULL}},
    {"validate", {"readonly", NULL}, {"bash", "execute_script", NULL}},
    {"current_code",
     {"git", NULL},
     {"bash", "execute_script", "read_file", "write_file", "edit_file", "list_files", "verify",
      "grep", "env_get", "test", NULL}},
    {"code",
     {"core", "git", "web", NULL},
     {"bash", "execute_script", "write_file", "edit_file", "verify", "env_get", "test",
      "run_background_process", "get_background_output", "kill_background_process",
      "list_background_processes", NULL}},
    {"review", {"readonly", NULL}, {"record_attempt", NULL}},
    /* Index-only review: the reviewer works from the caller-provided diff (in the
     * prompt) plus aimee's branch-indexed capabilities — NO filesystem/git tools,
     * which point at a worktree a remote delegate cannot reach. */
    {"review_indexed",
     {NULL},
     {"code_search", "find_symbol", "search_memory", "search_docs", "record_attempt", NULL}},
    {"script_rpc",
     {NULL},
     {"read_file", "list_files", "grep", "git_status", "git_log", "git_diff", "code_search",
      "find_symbol", "search_memory", "search_docs", "request_input", NULL}},
    {"full_stack", {"code", "review", "git", NULL}, {NULL}},
    {NULL, {NULL}, {NULL}},
};

static const char *const KNOWN_TOOLS[] = {
    "bash",
    "execute_script",
    "read_file",
    "write_file",
    "edit_file",
    "list_files",
    "verify",
    "git_log",
    "grep",
    "git_diff",
    "git_status",
    "env_get",
    "test",
    "request_input",
    "code_search",
    "web_search",
    "create_note",
    "list_notes",
    "search_notes",
    "run_background_process",
    "get_background_output",
    "kill_background_process",
    "list_background_processes",
    "search_docs",
    "find_symbol",
    "search_memory",
    "record_attempt",
    "respond",
    NULL,
};

static void toolset_err(char *err, size_t err_len, const char *fmt, ...)
{
   if (!err || err_len == 0)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(err, err_len, fmt, ap);
   va_end(ap);
}

static int name_valid(const char *name)
{
   if (!name || !name[0])
      return 0;
   for (const char *p = name; *p; p++)
   {
      if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
         return 0;
   }
   return 1;
}

int toolset_tool_known(const char *name)
{
   if (!name || !name[0])
      return 0;
   if (strchr(name, ':') != NULL)
      return 1;
   for (int i = 0; KNOWN_TOOLS[i]; i++)
      if (strcmp(KNOWN_TOOLS[i], name) == 0)
         return 1;
   return 0;
}

const toolset_def_t *toolset_registry_find(const toolset_registry_t *registry, const char *name)
{
   if (!registry || !name || !name[0])
      return NULL;
   for (int i = 0; i < registry->count; i++)
      if (strcmp(registry->sets[i].name, name) == 0)
         return &registry->sets[i];
   return NULL;
}

static toolset_def_t *toolset_registry_upsert(toolset_registry_t *registry, const char *name)
{
   if (!registry || !name_valid(name))
      return NULL;
   for (int i = 0; i < registry->count; i++)
      if (strcmp(registry->sets[i].name, name) == 0)
         return &registry->sets[i];
   if (registry->count >= TOOLSET_MAX_SETS)
      return NULL;
   toolset_def_t *def = &registry->sets[registry->count++];
   memset(def, 0, sizeof(*def));
   snprintf(def->name, sizeof(def->name), "%s", name);
   return def;
}

static void toolset_add_tool(toolset_def_t *def, const char *tool)
{
   if (!def || !tool || !tool[0] || def->tool_count >= TOOLSET_MAX_TOOLS)
      return;
   for (int i = 0; i < def->tool_count; i++)
      if (strcmp(def->tools[i], tool) == 0)
         return;
   snprintf(def->tools[def->tool_count++], TOOLSET_TOOL_MAX, "%s", tool);
}

static void toolset_add_include(toolset_def_t *def, const char *include)
{
   if (!def || !include || !include[0] || def->include_count >= TOOLSET_MAX_INCLUDE)
      return;
   for (int i = 0; i < def->include_count; i++)
      if (strcmp(def->include[i], include) == 0)
         return;
   snprintf(def->include[def->include_count++], TOOLSET_NAME_MAX, "%s", include);
}

void toolset_registry_init(toolset_registry_t *registry)
{
   if (!registry)
      return;
   memset(registry, 0, sizeof(*registry));
   snprintf(registry->script_allowed_tools, sizeof(registry->script_allowed_tools), "script_rpc");
   for (int i = 0; BUILTINS[i].name; i++)
   {
      toolset_def_t *def = toolset_registry_upsert(registry, BUILTINS[i].name);
      if (!def)
         continue;
      def->builtin = 1;
      for (int j = 0; BUILTINS[i].include[j]; j++)
         toolset_add_include(def, BUILTINS[i].include[j]);
      for (int j = 0; BUILTINS[i].tools[j]; j++)
         toolset_add_tool(def, BUILTINS[i].tools[j]);
   }
}

static int add_string_list(cJSON *node, void (*add)(toolset_def_t *, const char *),
                           toolset_def_t *def, char *err, size_t err_len)
{
   if (!node)
      return 0;
   if (cJSON_IsString(node))
   {
      char tmp[1024];
      snprintf(tmp, sizeof(tmp), "%s", node->valuestring);
      for (char *p = tmp; p && *p;)
      {
         while (*p == ' ' || *p == ',')
            p++;
         char *end = strchr(p, ',');
         if (end)
            *end = '\0';
         char *tail = p + strlen(p);
         while (tail > p && isspace((unsigned char)tail[-1]))
            *--tail = '\0';
         if (*p)
            add(def, p);
         p = end ? end + 1 : NULL;
      }
      return 0;
   }
   if (!cJSON_IsArray(node))
   {
      toolset_err(err, err_len, "toolset '%s' field must be string or list", def->name);
      return -1;
   }
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, node)
   {
      if (!cJSON_IsString(item) || !item->valuestring[0])
      {
         toolset_err(err, err_len, "toolset '%s' list item must be a string", def->name);
         return -1;
      }
      add(def, item->valuestring);
   }
   return 0;
}

static void toolset_registry_prune_unknown_tools(toolset_registry_t *registry)
{
   if (!registry)
      return;
   for (int i = 0; i < registry->count; i++)
   {
      toolset_def_t *def = &registry->sets[i];
      int dst = 0;
      for (int j = 0; j < def->tool_count; j++)
      {
         if (!toolset_tool_known(def->tools[j]))
         {
            LOG_WARN("toolset", "toolset '%s' references unknown tool '%s'; dropping", def->name,
                     def->tools[j]);
            continue;
         }
         if (dst != j)
            snprintf(def->tools[dst], TOOLSET_TOOL_MAX, "%s", def->tools[j]);
         dst++;
      }
      def->tool_count = dst;
   }
}

int toolset_registry_load_file(toolset_registry_t *registry, const char *path, char *err,
                               size_t err_len)
{
   if (!registry || !path || !path[0])
      return 0;
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (len < 0 || len > 1024 * 1024)
   {
      fclose(f);
      toolset_err(err, err_len, "toolset config too large: %s", path);
      return -1;
   }
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      toolset_err(err, err_len, "out of memory reading %s", path);
      return -1;
   }
   size_t nread = fread(buf, 1, (size_t)len, f);
   buf[nread] = '\0';
   fclose(f);

   cJSON *root = yaml_parse(buf);
   free(buf);
   if (!root)
   {
      toolset_err(err, err_len, "failed to parse toolset config: %s", path);
      return -1;
   }
   cJSON *sets = cJSON_GetObjectItemCaseSensitive(root, "toolsets");
   if (sets && !cJSON_IsObject(sets))
   {
      cJSON_Delete(root);
      toolset_err(err, err_len, "toolsets must be a mapping");
      return -1;
   }
   for (cJSON *item = sets ? sets->child : NULL; item; item = item->next)
   {
      if (!name_valid(item->string) || !cJSON_IsObject(item))
      {
         cJSON_Delete(root);
         toolset_err(err, err_len, "invalid toolset definition '%s'",
                     item->string ? item->string : "");
         return -1;
      }
      toolset_def_t *def = toolset_registry_upsert(registry, item->string);
      if (!def)
      {
         cJSON_Delete(root);
         toolset_err(err, err_len, "too many toolsets");
         return -1;
      }
      def->include_count = 0;
      def->tool_count = 0;
      if (add_string_list(cJSON_GetObjectItemCaseSensitive(item, "include"), toolset_add_include,
                          def, err, err_len) != 0 ||
          add_string_list(cJSON_GetObjectItemCaseSensitive(item, "tools"), toolset_add_tool, def,
                          err, err_len) != 0)
      {
         cJSON_Delete(root);
         return -1;
      }
   }
   cJSON *script = cJSON_GetObjectItemCaseSensitive(root, "script");
   if (script)
   {
      if (!cJSON_IsObject(script))
      {
         cJSON_Delete(root);
         toolset_err(err, err_len, "script must be a mapping");
         return -1;
      }
      cJSON *allowed = cJSON_GetObjectItemCaseSensitive(script, "allowed_tools");
      if (allowed)
      {
         if (!cJSON_IsString(allowed) || !name_valid(allowed->valuestring))
         {
            cJSON_Delete(root);
            toolset_err(err, err_len, "script.allowed_tools must be a toolset name");
            return -1;
         }
         snprintf(registry->script_allowed_tools, sizeof(registry->script_allowed_tools), "%s",
                  allowed->valuestring);
      }
   }
   cJSON_Delete(root);
   toolset_registry_prune_unknown_tools(registry);
   return toolset_registry_validate(registry, err, err_len);
}

static int resolve_index(const toolset_registry_t *registry, int idx, char out[][TOOLSET_TOOL_MAX],
                         int max_tools, int *count, int *visiting, int *visited, char *err,
                         size_t err_len)
{
   if (visited[idx])
      return 0;
   if (visiting[idx])
   {
      toolset_err(err, err_len, "toolset include cycle involving '%s'", registry->sets[idx].name);
      return -1;
   }
   visiting[idx] = 1;
   const toolset_def_t *def = &registry->sets[idx];
   for (int i = 0; i < def->include_count; i++)
   {
      int child = -1;
      for (int j = 0; j < registry->count; j++)
         if (strcmp(registry->sets[j].name, def->include[i]) == 0)
            child = j;
      if (child < 0)
      {
         toolset_err(err, err_len, "toolset '%s' includes unknown toolset '%s'", def->name,
                     def->include[i]);
         return -1;
      }
      if (resolve_index(registry, child, out, max_tools, count, visiting, visited, err, err_len) !=
          0)
         return -1;
   }
   for (int i = 0; i < def->tool_count; i++)
   {
      if (!toolset_tool_known(def->tools[i]))
      {
         LOG_WARN("toolset", "toolset '%s' references unknown tool '%s'; dropping", def->name,
                  def->tools[i]);
         continue;
      }
      int seen = 0;
      for (int j = 0; j < *count; j++)
         if (strcmp(out[j], def->tools[i]) == 0)
            seen = 1;
      if (!seen && *count < max_tools)
         snprintf(out[(*count)++], TOOLSET_TOOL_MAX, "%s", def->tools[i]);
   }
   visiting[idx] = 0;
   visited[idx] = 1;
   return 0;
}

static int cmp_tool_name(const void *a, const void *b)
{
   const char *sa = (const char *)a;
   const char *sb = (const char *)b;
   return strcmp(sa, sb);
}

int toolset_resolve(const toolset_registry_t *registry, const char *name,
                    char out[][TOOLSET_TOOL_MAX], int max_tools, char *err, size_t err_len)
{
   if (!registry || !name || !name[0] || !out || max_tools <= 0)
      return -1;
   int idx = -1;
   for (int i = 0; i < registry->count; i++)
      if (strcmp(registry->sets[i].name, name) == 0)
         idx = i;
   if (idx < 0)
   {
      toolset_err(err, err_len, "unknown toolset '%s'", name);
      return -1;
   }
   int visiting[TOOLSET_MAX_SETS] = {0};
   int visited[TOOLSET_MAX_SETS] = {0};
   int count = 0;
   if (resolve_index(registry, idx, out, max_tools, &count, visiting, visited, err, err_len) != 0)
      return -1;
   qsort(out, (size_t)count, TOOLSET_TOOL_MAX, cmp_tool_name);
   return count;
}

int toolset_registry_validate(const toolset_registry_t *registry, char *err, size_t err_len)
{
   if (!registry)
      return -1;
   char resolved[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   for (int i = 0; i < registry->count; i++)
      if (toolset_resolve(registry, registry->sets[i].name, resolved, TOOLSET_MAX_TOOLS, err,
                          err_len) < 0)
         return -1;
   if (!toolset_registry_find(registry, registry->script_allowed_tools))
   {
      toolset_err(err, err_len, "script.allowed_tools references unknown toolset '%s'",
                  registry->script_allowed_tools);
      return -1;
   }
   return 0;
}

int toolset_registry_load_effective(toolset_registry_t *registry, char *err, size_t err_len)
{
   if (!registry)
      return -1;
   toolset_registry_init(registry);
   const char *override = getenv("AIMEE_TOOLSETS_CONFIG");
   if (override && override[0])
      return toolset_registry_load_file(registry, override, err, err_len);
   static char path[MAX_PATH_LEN];
   const char *home = aimee_home();
   if (!home || !home[0])
      return 0;
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);
   return toolset_registry_load_file(registry, path, err, err_len);
}

int toolset_resolve_effective(const char *name, char out[][TOOLSET_TOOL_MAX], int max_tools,
                              char *err, size_t err_len)
{
   toolset_registry_t registry;
   if (toolset_registry_load_effective(&registry, err, err_len) != 0)
      return -1;
   return toolset_resolve(&registry, name, out, max_tools, err, err_len);
}

const char *toolset_for_delegate_role(const char *role)
{
   if (!role || !role[0])
      return NULL;
   if (strcmp(role, "implement") == 0 || strcmp(role, "build") == 0)
      role = "code";
   else if (strcmp(role, "test") == 0 || strcmp(role, "check") == 0 ||
            strcmp(role, "verifier") == 0 || strcmp(role, "evaluate") == 0 ||
            strcmp(role, "evaluate-optimize") == 0)
      role = "validate";
   else if (strcmp(role, "inspect") == 0)
      role = "diagnose";
   else if (strcmp(role, "research") == 0 || strcmp(role, "enforce") == 0)
      role = "execute";
   else if (strcmp(role, "recall") == 0)
      role = "search";
   else if (strcmp(role, "reviewer") == 0)
      role = "review";
   if (strcmp(role, "review") == 0 || strcmp(role, "diagnose") == 0)
      return "current_code";
   if (strcmp(role, "validate") == 0)
      return "validate";
   if (strcmp(role, "search") == 0)
      return "readonly";
   if (strcmp(role, "code") == 0 || strcmp(role, "refactor") == 0 || strcmp(role, "execute") == 0)
      return "full_stack";
   return NULL;
}
