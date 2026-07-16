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
    /* The code-intelligence tools (index_find_callers, index_blast_radius, ...) are
     * NOT listed here: they are declared native in the server's MCP table and land in
     * `core` and `review_indexed` by registration. One declaration, one schema, one
     * handler shared with external MCP clients. */
    {"core",
     {NULL},
     {"read_file", "list_files", "grep", "code_search", "find_symbol", "search_memory",
      /* aimee condenses long tool output and leaves a '[... ref "tc-..."]' pointer.
       * The tool that expands it was advertised to every agent (TSURF_ALL) and named
       * by no toolset, so agent_tools_filter_for_role stripped it from every role
       * that has one: agents were handed a ref they had no tool to redeem. It
       * belongs in core because the condensation applies to every tool. */
      "tool_output_get", NULL}},
    {"git", {NULL}, {"git_status", "git_log", "git_diff", NULL}},
    /* Git WRITES, deliberately split from the read-only `git` set above: `git` is
     * inherited by `readonly` (and thence review), which must never gain the power
     * to commit or push. Only `code` pulls this in.
     *
     * Without it a delegate has no way to land work but `bash git` — the very thing
     * require_aimee_git forbids. Adding the tools to the builtin registry is not
     * enough on its own: agent_tools_filter_for_role strips anything the role's
     * toolset does not name, so the rule would point at tools the filter hides. */
    {"git_write", {NULL}, {"git_commit", "git_push", "git_branch", "git_pr", NULL}},
    {"web", {NULL}, {"web_search", NULL}},
    {"readonly", {"core", "git", NULL}, {"verify", "env_get", "test", NULL}},
    {"validate", {"readonly", NULL}, {"bash", "execute_script", NULL}},
    {"current_code",
     {"git", NULL},
     {"bash", "execute_script", "read_file", "write_file", "edit_file", "list_files", "verify",
      "grep", "env_get", "test", NULL}},
    {"code",
     {"core", "git", "git_write", "web", NULL},
     {"bash", "execute_script", "write_file", "edit_file", "verify", "env_get", "test",
      "run_background_process", "get_background_output", "kill_background_process",
      "list_background_processes", NULL}},
    {"review", {"readonly", NULL}, {"record_attempt", NULL}},
    /* Index-only review: the reviewer works from the caller-provided diff (in the
     * prompt) plus aimee's branch-indexed capabilities. It also carries the
     * read-only worktree tools (read_file/list_files/grep) so a panelist can open
     * the file a diff is in — but those are REACHABILITY-GATED in
     * agent_tools_tool_allowed_for_role: granted only when the active workspace
     * provider can see the review worktree (SHARED/CONTAINER/MIRROR), and withheld
     * on a DETACHED remote seat whose read would hit the client's fs instead. It
     * never gains write_file/edit_file/bash: a reviewer must not edit what it judges. */
    {"review_indexed",
     {NULL},
     {"read_file", "list_files", "grep", "code_search", "find_symbol", "search_memory",
      "search_docs", "record_attempt", NULL}},
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
    /* Git writes. This list is a NAME ALLOWLIST, checked independently of the
     * builtin tool registry — a toolset naming a tool that is not here is pruned
     * with a warning, which is exactly how git_write silently lost all four and a
     * live delegate reported "there is no git_commit tool in my available toolset".
     * Adding a tool means agreeing in three places: the builtin registry, a
     * toolset, and here. */
    "git_commit",
    "git_push",
    "git_branch",
    "git_pr",
    "env_get",
    "test",
    "request_input",
    "code_search",
    "tool_output_get",
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

/* Tools registered from the MCP table at startup (see toolset_register_native_tool).
 * Sized to hold every MCP tool: the intended end state is that most of them are
 * native, so the table growing must never silently truncate. */
typedef struct
{
   char name[TOOLSET_TOOL_MAX];
   char toolset[TOOLSET_NAME_MAX];
} registered_tool_t;

static registered_tool_t g_registered[256];
static int g_registered_count;

void toolset_register_native_tool(const char *name, const char *toolset)
{
   if (!name || !name[0] || !toolset || !toolset[0])
      return;
   /* Dedup on the (tool, set) PAIR, not the name: one tool legitimately belongs to
    * several sets — the code-intelligence tools go to coding roles and to review
    * panelists both, and those sets do not include one another. */
   for (int i = 0; i < g_registered_count; i++)
      if (strcmp(g_registered[i].name, name) == 0 && strcmp(g_registered[i].toolset, toolset) == 0)
         return; /* idempotent: re-registration is not an error */
   if (g_registered_count >= (int)(sizeof(g_registered) / sizeof(g_registered[0])))
   {
      /* Loud: a dropped tool is exactly the silent-uncallable failure this
       * registry exists to end. */
      LOG_ERROR("toolset", "native tool registry full (%d); '%s' DROPPED and uncallable",
                g_registered_count, name);
      return;
   }
   registered_tool_t *r = &g_registered[g_registered_count++];
   snprintf(r->name, sizeof(r->name), "%s", name);
   snprintf(r->toolset, sizeof(r->toolset), "%s", toolset);
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
   for (int i = 0; i < g_registered_count; i++)
      if (strcmp(g_registered[i].name, name) == 0)
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
   /* Fold in the tools the server registered from the MCP table. A registration
    * naming a set that does not exist is a typo in the MCP table, not a reason to
    * invent a set: say so rather than create an orphan nothing includes. */
   for (int i = 0; i < g_registered_count; i++)
   {
      if (!toolset_registry_find(registry, g_registered[i].toolset))
      {
         LOG_ERROR("toolset", "native tool '%s' names unknown toolset '%s'; not callable",
                   g_registered[i].name, g_registered[i].toolset);
         continue;
      }
      toolset_def_t *def = toolset_registry_upsert(registry, g_registered[i].toolset);
      if (def)
         toolset_add_tool(def, g_registered[i].name);
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
   /* review works index-only: the change under review reaches it as a diff in the
    * prompt, and it navigates the current repo via the branch-indexed tools rather
    * than sweeping a worktree a remote delegate may not even be able to reach. */
   if (strcmp(role, "review") == 0)
      return "review_indexed";
   if (strcmp(role, "diagnose") == 0)
      return "current_code";
   if (strcmp(role, "validate") == 0)
      return "validate";
   if (strcmp(role, "search") == 0)
      return "readonly";
   if (strcmp(role, "code") == 0 || strcmp(role, "refactor") == 0 || strcmp(role, "execute") == 0)
      return "full_stack";
   return NULL;
}
