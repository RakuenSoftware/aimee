/* agent_tools.c: tool execution, checkpoints, and tool definition JSON builders */
#include "aimee.h"
#include "util.h"
#include "agent_tools.h"
#include "agent_exec.h"
#include "workspace.h"
#include "computer_use.h"
#include "config.h"
#include "diff.h"
#include "dstr.h"
#include "guardrails.h"
#include "headers/mcp_client_registry.h"
#include "log.h"
#include "toolset.h"
#include "tool_schema_sanitizer.h"
#include "cJSON.h"
#include <ctype.h>
#include <pthread.h>

/* --- Auto-snapshot turn context (thread-local, set by the agent runtime) ---
 *
 * The agent runtime calls agent_tools_begin_turn() at the start of each
 * tool-call round so that all write_file / edit_file calls within that round
 * share a single fsnap snapshot (visible in `aimee rewind list`). */
_Thread_local static int g_aturn_turn = -1;      /* current turn (-1 = unknown) */
_Thread_local static int64_t g_aturn_snapid = 0; /* cached snap id for this turn */
_Thread_local static char g_aturn_sid[64] = {0}; /* session_id copy */

void agent_tools_begin_turn(int turn)
{
   const char *sid = session_id();
   g_aturn_turn = turn;
   g_aturn_snapid = 0; /* reset: new snapshot will be created on first write */
   if (sid)
      snprintf(g_aturn_sid, sizeof(g_aturn_sid), "%s", sid);
   else
      g_aturn_sid[0] = '\0';
}

int agent_tools_get_turn(void)
{
   return g_aturn_turn;
}
int64_t agent_tools_get_snap_id(void)
{
   return g_aturn_snapid;
}
void agent_tools_set_snap_id(int64_t id)
{
   g_aturn_snapid = id;
}
_Thread_local static char g_parent_ro_root[MAX_PATH_LEN] = {0};
_Thread_local static char g_parent_write_root[MAX_PATH_LEN] = {0};
/* Backend-agnostic read-only-delegate gate. 1 = writes permitted (the default,
 * so primary sessions and any non-delegate caller are unaffected); 0 = this
 * delegate is not write-capable and ALL file writes are refused. Set per
 * delegation from the delegate's write policy; reset on guard clear. */
_Thread_local static int g_write_capable = 1;

static int agent_tools_path_under_root(const char *path, const char *root)
{
   if (!path || !path[0] || !root || !root[0])
      return 0;
   size_t root_len = strlen(root);
   if (strncmp(path, root, root_len) != 0)
      return 0;
   return path[root_len] == '\0' || path[root_len] == '/';
}

static void agent_tools_normalize_guard_root(const char *path, char *dst, size_t dst_len)
{
   if (!path || !path[0] || dst_len == 0)
   {
      if (dst_len > 0)
         dst[0] = '\0';
      return;
   }

   normalize_path(path, NULL, dst, dst_len);
   size_t len = strlen(dst);
   while (len > 1 && dst[len - 1] == '/')
      dst[--len] = '\0';
}

void agent_tools_parent_write_guard_set(const char *read_only_root, const char *write_root)
{
   if (read_only_root && read_only_root[0])
      agent_tools_normalize_guard_root(read_only_root, g_parent_ro_root, sizeof(g_parent_ro_root));
   else
      g_parent_ro_root[0] = '\0';

   if (write_root && write_root[0])
      agent_tools_normalize_guard_root(write_root, g_parent_write_root,
                                       sizeof(g_parent_write_root));
   else
      g_parent_write_root[0] = '\0';

   if (g_parent_ro_root[0] && g_parent_write_root[0] &&
       strcmp(g_parent_ro_root, g_parent_write_root) == 0)
   {
      /* Delegates that share the session worktree have no separate parent
       * root to protect here. The normal tool-mode write policy still
       * decides whether read-only tools may write. */
      g_parent_ro_root[0] = '\0';
      g_parent_write_root[0] = '\0';
   }
}

void agent_tools_parent_write_guard_clear(void)
{
   g_parent_ro_root[0] = '\0';
   g_parent_write_root[0] = '\0';
   g_write_capable = 1;
}

void agent_tools_write_capable_set(int capable)
{
   g_write_capable = capable ? 1 : 0;
}

int agent_tools_readonly_delegate_blocks(void)
{
   return !g_write_capable;
}

const char *agent_tools_parent_write_guard_root(void)
{
   return g_parent_ro_root[0] ? g_parent_ro_root : NULL;
}

const char *agent_tools_parent_write_guard_write_root(void)
{
   return g_parent_write_root[0] ? g_parent_write_root : NULL;
}

int agent_tools_parent_write_guard_blocks(const char *path, const char *cwd)
{
   if (!g_parent_ro_root[0] || !path || !path[0])
      return 0;

   char norm[MAX_PATH_LEN];
   normalize_path(path, cwd, norm, sizeof(norm));
   if (g_parent_write_root[0] && agent_tools_path_under_root(norm, g_parent_write_root))
      return 0;
   return agent_tools_path_under_root(norm, g_parent_ro_root);
}

/* Session-isolation backstop (Layer 2, opt-in via require_session_worktree).
 * Blocks a server-side agent/delegate write whose normalized target is NOT
 * inside an aimee-managed worktree (path component .aimee/worktrees/...). This
 * mirrors the client-side attention-guard
 * (Layer 1) so aimee's own in-process agent writes obey the same isolation
 * policy that a thin client's PreToolUse hook enforces — covering the case
 * where session-start never provisioned a worktree. Default off (the config
 * flag defaults to 0), so this is a no-op unless explicitly enabled. */
int agent_tools_session_isolation_blocks(const char *path, const char *cwd)
{
   if (!path || !path[0])
      return 0;
   /* config_load here mirrors the per-call pattern already used elsewhere in
    * this file (it is cheap and reads the cached config). Default-off: when the
    * flag is unset — or the config is unreadable, which leaves the default 0 —
    * this is a no-op, matching the feature's opt-in nature. */
   config_t cfg;
   config_load(&cfg);
   if (!cfg.require_session_worktree)
      return 0;
   /* normalize_path resolves '.'/'..'/relative against cwd, closing traversal
    * escapes. Match the canonical managed-worktree location plus the workflow
    * engine's per-work-item worktrees (not the looser "/.aimee-" prefix
    * is_aimee_worktree_path() accepts), consistent with the client-side
    * attn_session_isolation_blocked check. /wfe-worktrees/ is the same fix
    * #1314 applied to the guardrail layer: a wfe delegate's target IS an
    * isolated worktree — without it, this backstop (default ON) refused every
    * native write_file/edit_file into a wfe worktree, so implement delegates
    * that edit via file tools produced no-op rounds while bash-writers slipped
    * through — the whole fleet's convergence depended on each model's tool
    * preferences. */
   char norm[MAX_PATH_LEN];
   normalize_path(path, cwd, norm, sizeof(norm));
   if (strstr(norm, "/.aimee/worktrees/") != NULL)
      return 0;
   if (strstr(norm, "/wfe-worktrees/") != NULL)
      return 0;
   return 1;
}

static int tools_array_has_name(cJSON *tools, const char *name, int openai_format)
{
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      cJSON *name_item = openai_format
                             ? cJSON_GetObjectItem(cJSON_GetObjectItem(tool, "function"), "name")
                             : cJSON_GetObjectItem(tool, "name");
      if (cJSON_IsString(name_item) && strcmp(name_item->valuestring, name) == 0)
         return 1;
   }
   return 0;
}

static void append_remote_tools(cJSON *tools, int openai_format)
{
   config_t cfg;
   config_load(&cfg);
   computer_use_policy_t cu_policy;
   computer_use_policy_from_config(&cfg, &cu_policy);

   cJSON *remote_tools = mcp_client_registry_build_namespaced_tools(1000);
   if (!cJSON_IsArray(remote_tools))
   {
      cJSON_Delete(remote_tools);
      return;
   }

   cJSON *remote_tool = NULL;
   cJSON_ArrayForEach(remote_tool, remote_tools)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(remote_tool, "name");
      if (!cJSON_IsString(name) || !name->valuestring[0])
         continue;
      if (computer_use_is_tool_name(name->valuestring) && !cu_policy.enabled)
         continue;

      const char *raw_name = strchr(name->valuestring, ':');
      if (raw_name && raw_name[1] && tools_array_has_name(tools, raw_name + 1, openai_format))
         LOG_WARN("mcp-tools", "remote tool name collides with builtin, namespaced as %s",
                  name->valuestring);

      cJSON *desc = cJSON_GetObjectItemCaseSensitive(remote_tool, "description");
      cJSON *schema = cJSON_GetObjectItemCaseSensitive(remote_tool, "inputSchema");
      if (!schema)
      {
         schema = cJSON_CreateObject();
         cJSON_AddStringToObject(schema, "type", "object");
         cJSON_AddObjectToObject(schema, "properties");
      }
      else
      {
         schema = cJSON_Duplicate(schema, 1);
      }
      if (!schema)
         continue;

      if (openai_format)
      {
         cJSON *tool = cJSON_CreateObject();
         cJSON *fn = cJSON_CreateObject();
         cJSON_AddStringToObject(tool, "type", "function");
         cJSON_AddStringToObject(fn, "name", name->valuestring);
         cJSON_AddStringToObject(fn, "description", cJSON_IsString(desc) ? desc->valuestring : "");
         cJSON_AddItemToObject(fn, "parameters", schema);
         cJSON_AddItemToObject(tool, "function", fn);
         cJSON_AddItemToArray(tools, tool);
      }
      else
      {
         cJSON *tool = cJSON_CreateObject();
         cJSON_AddStringToObject(tool, "type", "function");
         cJSON_AddStringToObject(tool, "name", name->valuestring);
         cJSON_AddStringToObject(tool, "description",
                                 cJSON_IsString(desc) ? desc->valuestring : "");
         cJSON_AddItemToObject(tool, "parameters", schema);
         cJSON_AddItemToArray(tools, tool);
      }
   }

   cJSON_Delete(remote_tools);
}

cJSON *delegate_respond_spec(void)
{
   cJSON *tool = cJSON_CreateObject();
   cJSON *fn = cJSON_CreateObject();
   cJSON *params = cJSON_CreateObject();
   cJSON *props = cJSON_CreateObject();
   cJSON *message = cJSON_CreateObject();
   cJSON *required = cJSON_CreateArray();

   if (!tool || !fn || !params || !props || !message || !required)
   {
      cJSON_Delete(tool);
      cJSON_Delete(fn);
      cJSON_Delete(params);
      cJSON_Delete(props);
      cJSON_Delete(message);
      cJSON_Delete(required);
      return NULL;
   }

   cJSON_AddStringToObject(tool, "type", "function");
   cJSON_AddStringToObject(fn, "name", "respond");
   cJSON_AddStringToObject(fn, "description",
                           "Respond to the user with a message. Use this when chatting, asking a "
                           "clarifying question, or reporting the final result, whenever no other "
                           "tool action is needed.");
   cJSON_AddStringToObject(params, "type", "object");
   cJSON_AddStringToObject(message, "type", "string");
   cJSON_AddStringToObject(message, "description", "The message to return to the user");
   cJSON_AddItemToObject(props, "message", message);
   cJSON_AddItemToObject(params, "properties", props);
   cJSON_AddItemToArray(required, cJSON_CreateString("message"));
   cJSON_AddItemToObject(params, "required", required);
   cJSON_AddItemToObject(fn, "parameters", params);
   cJSON_AddItemToObject(tool, "function", fn);
   return tool;
}

int agent_tools_append_delegate_respond_tool(cJSON *tools)
{
   if (!cJSON_IsArray(tools))
      return 0;
   if (tools_array_has_name(tools, "respond", 1))
      return 0;
   cJSON *respond = delegate_respond_spec();
   if (!respond)
      return 0;
   cJSON_AddItemToArray(tools, respond);
   return 1;
}

static char *delegate_respond_message(const char *arguments_json)
{
   cJSON *root = arguments_json ? cJSON_Parse(arguments_json) : NULL;
   cJSON *msg = root ? cJSON_GetObjectItemCaseSensitive(root, "message") : NULL;
   char *out = safe_strdup(cJSON_IsString(msg) ? msg->valuestring : "");
   cJSON_Delete(root);
   return out;
}

int agent_tools_strip_delegate_respond(parsed_response_t *parsed)
{
   if (!parsed || !parsed->is_tool_call || parsed->call_count <= 0)
      return 0;

   int respond_count = 0;
   for (int i = 0; i < parsed->call_count; i++)
   {
      if (strcmp(parsed->calls[i].name, "respond") == 0)
         respond_count++;
   }
   if (respond_count == 0)
      return 0;

   if (respond_count == parsed->call_count)
   {
      char *message = delegate_respond_message(parsed->calls[0].arguments);
      for (int i = 0; i < parsed->call_count; i++)
      {
         free(parsed->calls[i].arguments);
         memset(&parsed->calls[i], 0, sizeof(parsed->calls[i]));
      }
      free(parsed->content);
      parsed->content = message ? message : safe_strdup("");
      parsed->call_count = 0;
      parsed->is_tool_call = 0;
      return 1;
   }

   int dst = 0;
   for (int i = 0; i < parsed->call_count; i++)
   {
      if (strcmp(parsed->calls[i].name, "respond") == 0)
      {
         free(parsed->calls[i].arguments);
         memset(&parsed->calls[i], 0, sizeof(parsed->calls[i]));
         continue;
      }
      if (dst != i)
      {
         parsed->calls[dst] = parsed->calls[i];
         memset(&parsed->calls[i], 0, sizeof(parsed->calls[i]));
      }
      dst++;
   }
   parsed->call_count = dst;
   parsed->is_tool_call = parsed->call_count > 0;
   return 2;
}

/* --- Tool execution (Unix only) --- */

/* Tool implementations are in posix/agent_tools.c (POSIX) and
 * windows/agent_tools.c (Windows). */

/* Tool definition builders are in agent_tools_defs.c */

int agent_tools_role_current_code_only(const char *role)
{
   if (!role || !role[0])
      return 0;
   return strcmp(role, "review") == 0 || strcmp(role, "diagnose") == 0 ||
          strcmp(role, "inspect") == 0;
}

int agent_tools_tool_allowed_for_role(const char *role, const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return 0;

   const char *toolset_name = getenv("AIMEE_ACTIVE_TOOLSET");
   if (!toolset_name || !toolset_name[0])
      toolset_name = toolset_for_delegate_role(role);
   if (toolset_name && toolset_name[0])
   {
      char tools[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
      char err[TOOLSET_ERROR_MAX] = "";
      int n = toolset_resolve_effective(toolset_name, tools, TOOLSET_MAX_TOOLS, err, sizeof(err));
      if (n >= 0)
      {
         for (int i = 0; i < n; i++)
            if (strcmp(tools[i], tool_name) == 0)
               return 1;
         return 0;
      }
      LOG_WARN("toolset", "failed to resolve toolset '%s': %s", toolset_name,
               err[0] ? err : "unknown error");
   }

   if (!agent_tools_role_current_code_only(role))
      return 1;
   if (strchr(tool_name, ':') != NULL)
      return 0;

   static const char *blocked[] = {
       "code_search",
       "find_symbol",
       "search_memory",
       "search_docs",
       "create_note",
       "list_notes",
       "search_notes",
       "rules_propose",
       "rules_list",
       "learning_propose",
       "learning_review",
       "clarify_start",
       "clarify_answer",
       "diagnose_start",
       "diagnose_observe",
       "diagnose_hypothesize",
       "diagnose_evidence",
       "diagnose_report",
       NULL,
   };
   for (int i = 0; blocked[i]; i++)
   {
      if (strcmp(tool_name, blocked[i]) == 0)
         return 0;
   }
   return 1;
}

static const char *tool_def_name(cJSON *tool)
{
   if (!tool)
      return NULL;
   cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
   if (cJSON_IsString(name))
      return name->valuestring;
   cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
   if (cJSON_IsObject(fn))
   {
      name = cJSON_GetObjectItemCaseSensitive(fn, "name");
      if (cJSON_IsString(name))
         return name->valuestring;
   }
   return NULL;
}

void agent_tools_filter_for_role(cJSON *tools, const char *role)
{
   const char *toolset_name = getenv("AIMEE_ACTIVE_TOOLSET");
   char resolved[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX];
   int resolved_count = -1;
   if (!toolset_name || !toolset_name[0])
      toolset_name = toolset_for_delegate_role(role);
   if (!cJSON_IsArray(tools) || (!toolset_name && !agent_tools_role_current_code_only(role)))
      return;
   if (toolset_name)
   {
      char err[TOOLSET_ERROR_MAX] = "";
      resolved_count =
          toolset_resolve_effective(toolset_name, resolved, TOOLSET_MAX_TOOLS, err, sizeof(err));
      if (resolved_count < 0)
      {
         LOG_WARN("toolset", "failed to resolve toolset '%s': %s", toolset_name,
                  err[0] ? err : "unknown error");
         resolved_count = 0;
      }
   }
   for (int i = 0; i < cJSON_GetArraySize(tools);)
   {
      cJSON *tool = cJSON_GetArrayItem(tools, i);
      const char *name = tool_def_name(tool);
      int in_toolset = resolved_count < 0;
      for (int j = 0; !in_toolset && name && j < resolved_count; j++)
         in_toolset = strcmp(name, resolved[j]) == 0;
      if (!in_toolset || !agent_tools_tool_allowed_for_role(role, name))
         cJSON_DeleteItemFromArray(tools, i);
      else
         i++;
   }
}

/* ================================================================
 * From: agent_tools_defs.c
 * ================================================================ */
/* ================================================================
 * Builtin tool definitions — single source of truth.
 *
 * Both OpenAI-shaped surfaces (Chat Completions and the Responses API) are
 * generated from the one ordered table below, so a tool's name, description,
 * and parameters are defined exactly once. Per-surface membership is declared
 * explicitly in the `surfaces` bitmask — the historical hand-maintained drift
 * between the two builders (code_search/execute_script were Chat-only,
 * search_docs Responses-only) is now visible in one place rather than split
 * across two ~500-line functions that had to be edited in lockstep.
 *
 * To keep this a pure refactor, the table preserves each surface's exact
 * current tool set and order. Whether the surface-specific tools *should* be
 * unified is a separate product decision (they are genuinely different tools —
 * code search vs. doc search — not a rename), tracked in
 * aimee-code-health-solid-dry-refactor.md.
 *
 * Anthropic format is still derived from build_tools_array() below.
 * ================================================================ */

#define TSURF_CHAT 0x1u /* build_tools_array — OpenAI Chat Completions */
#define TSURF_RESP 0x2u /* build_tools_array_responses — OpenAI Responses API */
#define TSURF_ALL  (TSURF_CHAT | TSURF_RESP)

typedef cJSON *(*tool_params_fn)(void);

typedef struct
{
   const char *name;
   const char *description;
   tool_params_fn params; /* returns a fresh JSON-Schema "parameters" object */
   unsigned surfaces;     /* TSURF_* bitmask: which surface(s) expose this tool */
} builtin_tool_def_t;

/* --- Parameter builders (one per tool; key-insertion order is load-bearing
 *     because it determines serialized JSON key order — keep it stable). --- */

static cJSON *tp_obj(void)
{
   cJSON *params = cJSON_CreateObject();
   cJSON_AddStringToObject(params, "type", "object");
   return params;
}

static void tp_prop(cJSON *props, const char *name, const char *type, const char *desc)
{
   cJSON *p = cJSON_CreateObject();
   cJSON_AddStringToObject(p, "type", type);
   cJSON_AddStringToObject(p, "description", desc);
   cJSON_AddItemToObject(props, name, p);
}

/* An array-of-strings property. Separate from tp_prop because a bare
 * {"type":"array"} is incomplete JSON Schema — `items` is what tells a provider
 * what the array holds, and strict ones reject the schema without it. */
static void tp_prop_str_array(cJSON *props, const char *name, const char *desc)
{
   cJSON *p = cJSON_CreateObject();
   cJSON_AddStringToObject(p, "type", "array");
   cJSON_AddStringToObject(p, "description", desc);
   cJSON *items = cJSON_CreateObject();
   cJSON_AddStringToObject(items, "type", "string");
   cJSON_AddItemToObject(p, "items", items);
   cJSON_AddItemToObject(props, name, p);
}

static cJSON *tp_bash(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "command", "string", "The shell command to execute");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("command"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_execute_script(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "language", "string", "Script language: python or bash");
   tp_prop(props, "body", "string", "Script source code to execute");
   tp_prop(props, "timeout_secs", "integer", "Timeout in seconds (default 120, max 600)");
   tp_prop(props, "workdir", "string",
           "Working directory (optional, defaults to current workspace)");
   tp_prop(props, "env", "object", "Additional environment variables after secret scrubbing");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("language"));
   cJSON_AddItemToArray(req, cJSON_CreateString("body"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_read_file(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string",
           "File path. Prefer paths relative to the current workspace directory.");
   tp_prop(props, "offset", "integer", "Line offset to start reading from");
   tp_prop(props, "limit", "integer", "Maximum number of lines to read");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_tool_output_get(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "ref", "string",
           "The spill ref from a '[output condensed by aimee ... ref \"tc-...\"]' pointer.");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("ref"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_write_file(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string",
           "File path to write. Prefer paths relative to the current workspace directory.");
   tp_prop(props, "content", "string", "Content to write");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToArray(req, cJSON_CreateString("content"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_edit_file(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string",
           "File path to edit. Prefer paths relative to the current workspace directory.");
   tp_prop(props, "old_string", "string",
           "Exact existing text to replace (must be unique unless replace_all).");
   tp_prop(props, "new_string", "string", "Replacement text.");
   tp_prop(props, "replace_all", "boolean",
           "Replace every occurrence instead of requiring uniqueness.");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToArray(req, cJSON_CreateString("old_string"));
   cJSON_AddItemToArray(req, cJSON_CreateString("new_string"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_list_files(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string",
           "Directory path to list. Prefer paths relative to the current workspace directory.");
   tp_prop(props, "pattern", "string", "Glob pattern to filter files");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_verify(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "check_type", "string", "http_status, file_contains, or command_succeeds");
   tp_prop(props, "target", "string", "URL, file path, or command to check");
   tp_prop(props, "expected", "string", "Expected value (optional)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("check_type"));
   cJSON_AddItemToArray(req, cJSON_CreateString("target"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_git_log(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string", "Path to the git repository");
   tp_prop(props, "count", "integer", "Number of commits (default 10)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_grep(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string", "Directory or file to search in");
   tp_prop(props, "pattern", "string", "Pattern to search for (basic regex)");
   tp_prop(props, "max_results", "integer", "Max results (default 50)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_git_diff(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string", "Path to the git repository");
   tp_prop(props, "ref", "string", "Git ref to diff against (optional)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

/* The git WRITE tools. These exist so a delegate has an aimee route to commit /
 * push / open a PR at all: before this, the native builtin set carried read-only
 * git (log/diff/status), so the ONLY way for a delegate to land work was shelling
 * out to `git` — which is exactly what require_aimee_git forbids. A rule with no
 * permitted alternative is just breakage, so the tools land first.
 *
 * They dispatch through mcp_git_run_tool, i.e. the same path an external MCP
 * client takes, so the safety rails (worktree refusal, branch ownership, the
 * verify gate, AI-attribution stripping) hold identically on both surfaces. */
static cJSON *tp_git_commit(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "message", "string", "Commit message");
   tp_prop(props, "path", "string",
           "Path to the git repository / worktree — the SAME path you pass to git_status. "
           "Without it the repo is resolved from the session, which may not be your worktree.");
   /* `files` is how anything gets STAGED — including a file you just created.
    * There is deliberately no add-everything flag: sensitive paths are screened
    * per file. Omit it and only already-staged changes are committed. */
   tp_prop_str_array(props, "files",
                     "Paths to stage before committing (git add -- <files>). REQUIRED for a new "
                     "or unstaged file; without it only already-staged changes are committed.");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("message"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_git_push(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string",
           "Path to the git repository / worktree — the SAME path you pass to git_status. "
           "Without it the repo is resolved from the session, which may not be your worktree.");
   /* No upstream arg: the handler pushes the current branch and sets upstream itself. */
   tp_prop(props, "force", "boolean", "Force-push with lease (default false)");
   tp_prop(props, "mirror", "boolean", "Push to the configured mirror remote (default false)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON_AddItemToObject(params, "required", cJSON_CreateArray());
   return params;
}

static cJSON *tp_git_branch(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "action", "string", "list | create | switch | claim | delete | orphan");
   tp_prop(props, "path", "string",
           "Path to the git repository / worktree — the SAME path you pass to git_status. "
           "Without it the repo is resolved from the session, which may not be your worktree.");
   tp_prop(props, "name", "string", "Branch name (for create/switch/claim/delete)");
   tp_prop(props, "base", "string", "Base ref for create (defaults to the current HEAD)");
   tp_prop(props, "remote", "boolean", "Include remote branches when listing");
   tp_prop(props, "force", "boolean", "Force the operation (e.g. delete an unmerged branch)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("action"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_git_pr(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "action", "string",
           "create | view | list | edit | checks | watch | merge_status | merge");
   tp_prop(props, "path", "string",
           "Path to the git repository / worktree — the SAME path you pass to git_status. "
           "Without it the repo is resolved from the session, which may not be your worktree.");
   tp_prop(props, "number", "integer", "PR number (for view/edit/checks/merge_status/merge)");
   tp_prop(props, "title", "string", "PR title (for create/edit)");
   tp_prop(props, "body", "string", "PR body (for create/edit)");
   tp_prop(props, "base", "string", "Base branch (for create/edit)");
   tp_prop(props, "merge_method", "string", "merge | squash | rebase (for merge; default merge)");
   tp_prop(props, "expected_head_sha", "string",
           "For merge: refuse if the PR head has moved from this SHA (drift safety)");
   tp_prop(props, "wait", "boolean", "For checks: poll until the checks settle");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("action"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_git_status(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string", "Path to the git repository");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_env_get(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "name", "string", "Environment variable name");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("name"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_test(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "path", "string", "Path to check");
   tp_prop(props, "check", "string",
           "Check type: exists, is_file, is_dir, readable, writable, executable");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("path"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_code_search(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "query", "string", "Search query");
   tp_prop(props, "project", "string", "Project name to search in (optional)");
   tp_prop(props, "max_results", "integer", "Max results (default 50)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("query"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_web_search(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "query", "string", "The search query");
   tp_prop(props, "max_results", "integer", "Maximum results to return (default 5, max 10)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("query"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_create_note(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "title", "string",
           "Note title. If a note with this title exists, content is appended.");
   tp_prop(props, "content", "string", "Markdown content for the note");
   tp_prop(props, "tags", "string", "Comma-separated tags (e.g. 'debugging,auth')");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("title"));
   cJSON_AddItemToArray(req, cJSON_CreateString("content"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_list_notes(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "tag", "string", "Filter notes by tag");
   tp_prop(props, "limit", "integer", "Maximum notes to return (default 20)");
   cJSON_AddItemToObject(params, "properties", props);
   return params;
}

static cJSON *tp_search_notes(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "query", "string", "Search term to find in note titles and content");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("query"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_run_background_process(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "command", "string", "Shell command to run in the background");
   tp_prop(props, "cwd", "string",
           "Working directory for the command (optional, defaults to current)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("command"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_get_background_output(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "id", "integer", "Process ID returned by run_background_process");
   tp_prop(props, "tail_lines", "integer",
           "Number of recent lines to return (default 50, max 500)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("id"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_kill_background_process(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "id", "integer", "Process ID to kill");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("id"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_list_background_processes(void)
{
   cJSON *params = tp_obj();
   cJSON_AddObjectToObject(params, "properties");
   return params;
}

static cJSON *tp_find_symbol(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "identifier", "string",
           "Symbol name to look up (e.g. 'kb_directive_request', 'memory_t', 'AIMEE_LOG')");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("identifier"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_search_memory(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "query", "string", "Search terms to find matching facts and memories");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("query"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

static cJSON *tp_search_docs(void)
{
   cJSON *params = tp_obj();
   cJSON *props = cJSON_CreateObject();
   tp_prop(props, "query", "string",
           "What you want to know about the project — a question or topic");
   tp_prop(props, "max_results", "integer", "Maximum passages to return (default 3, max 8)");
   cJSON_AddItemToObject(params, "properties", props);
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("query"));
   cJSON_AddItemToObject(params, "required", req);
   return params;
}

/* Ordered union of both surfaces. The 22 shared tools appear in the same
 * relative order both builders used; the surface-specific tools sit at their
 * original positions (execute_script after bash, code_search after test for
 * Chat; search_docs before find_symbol for Responses). Emitting in this order,
 * filtered by surface, reproduces each builder's prior output exactly. */
static const builtin_tool_def_t g_builtin_tools[] = {
    {"bash", "Run a shell command. Returns JSON with stdout, stderr, exit_code.", tp_bash,
     TSURF_ALL},
    {"execute_script",
     "Run a bounded Python or Bash script with a scrubbed environment. Returns JSON with "
     "stdout, stderr, exit_code, duration_ms, and truncation flags.",
     tp_execute_script, TSURF_CHAT},
    {"read_file", "Read a file and return its contents.", tp_read_file, TSURF_ALL},
    {"tool_output_get",
     "Retrieve the full, unfiltered output that aimee condensed. Pass the ref from a "
     "'[output condensed by aimee ... ref \"tc-...\"]' pointer to get the complete original "
     "output (e.g. a passing test case or an elided detail).",
     tp_tool_output_get, TSURF_ALL},
    {"write_file", "Write content to a file (overwrites).", tp_write_file, TSURF_ALL},
    {"edit_file",
     "Make a surgical edit to an existing file by replacing old_string with new_string. "
     "Prefer this over write_file when changing part of a file — you do not need to "
     "reproduce the whole file. old_string must match the file exactly (including "
     "whitespace/indentation) and be unique unless replace_all is true.",
     tp_edit_file, TSURF_ALL},
    {"list_files", "List files in a directory, optionally matching a glob pattern.", tp_list_files,
     TSURF_ALL},
    {"verify", "Verify an assertion. check_type: http_status, file_contains, command_succeeds.",
     tp_verify, TSURF_ALL},
    {"git_log", "Show recent git commits for a repository.", tp_git_log, TSURF_ALL},
    {"grep", "Search for a pattern in files. Returns matching lines with file:line.", tp_grep,
     TSURF_ALL},
    {"git_diff", "Show git diff for a repository. Optionally diff against a ref.", tp_git_diff,
     TSURF_ALL},
    {"git_status", "Show git status (porcelain format) for a repository.", tp_git_status,
     TSURF_ALL},
    /* Git writes go through these, never through `bash`: they run on aimee-server,
     * where the forge credential is resolved per call and stays in process memory
     * instead of reaching a child's environment. They also carry the rails a raw
     * `git` command has no idea about — branch ownership, the verify gate, and
     * attribution stripping. Appended after the read-only git tools so the
     * pre-existing surface order (which the tools/list golden pins) is untouched. */
    {"git_commit",
     "Commit staged (or, with add_all, all) changes in the session worktree. Use this instead "
     "of running `git commit` in a shell.",
     tp_git_commit, TSURF_ALL},
    {"git_push",
     "Push the current branch to the forge. Authenticates on aimee-server; you do not need "
     "(and will not have) git credentials. Use this instead of running `git push`.",
     tp_git_push, TSURF_ALL},
    {"git_branch", "List, create, claim, or show the current git branch.", tp_git_branch,
     TSURF_ALL},
    {"git_pr",
     "Create, view, list, edit, or check a pull request. Use this instead of running `gh`.",
     tp_git_pr, TSURF_ALL},
    {"env_get", "Get the value of an environment variable.", tp_env_get, TSURF_ALL},
    {"test",
     "Check file/dir existence, type, and permissions. "
     "check: exists, is_file, is_dir, readable, writable, executable.",
     tp_test, TSURF_ALL},
    {"code_search",
     "Lexical search across indexed code files. Returns ranked results with snippets.",
     tp_code_search, TSURF_CHAT},
    {"web_search",
     "Search the web for current documentation, error messages, or API "
     "references. Returns titles, URLs, and snippets for the top results.",
     tp_web_search, TSURF_ALL},
    {"create_note",
     "Create or append to an investigation note. Notes capture findings, hypotheses, "
     "and reasoning during debugging. If a note with the same title already exists, "
     "the new content is appended.",
     tp_create_note, TSURF_ALL},
    {"list_notes",
     "List investigation notes, optionally filtered by tag. "
     "Returns titles, tags, and update times.",
     tp_list_notes, TSURF_ALL},
    {"search_notes",
     "Search investigation notes by content or title substring. "
     "Returns matching notes with their content.",
     tp_search_notes, TSURF_ALL},
    {"run_background_process",
     "Start a shell command in the background. Returns a process ID. "
     "Use get_background_output to read its output and "
     "kill_background_process to stop it.",
     tp_run_background_process, TSURF_ALL},
    {"get_background_output",
     "Get recent stdout/stderr output from a background process. "
     "Returns the last tail_lines lines of combined output.",
     tp_get_background_output, TSURF_ALL},
    {"kill_background_process", "Send SIGTERM to a running background process and mark it exited.",
     tp_kill_background_process, TSURF_ALL},
    {"list_background_processes",
     "List all background processes with their status, PID, command, "
     "and exit code. Returns a JSON array.",
     tp_list_background_processes, TSURF_ALL},
    {"search_docs",
     "Search project documentation for relevant context. Use when you "
     "need to understand project architecture, APIs, design decisions, "
     "or domain concepts. Returns matching passages with source attribution. "
     "Requires the documentation index to be available.",
     tp_search_docs, TSURF_RESP},
    {"find_symbol",
     "Look up the exact definition location of a C symbol, function, type, or macro in the "
     "indexed codebase. Returns file:line matches. Prefer this over grep for finding where "
     "something is defined or which header to include.",
     tp_find_symbol, TSURF_ALL},
    {"search_memory",
     "Search aimee's knowledge base for stored facts, prior decisions, and project context. "
     "Use before starting any task to check for relevant prior work or constraints.",
     tp_search_memory, TSURF_ALL},
};

/* Emit the builtin tools selected by `surface` into `tools`, in table order,
 * using that surface's JSON shape:
 *   TSURF_CHAT: {"type":"function","function":{name,description,parameters}}
 *   TSURF_RESP: {"type":"function",name,description,parameters}            */
/* The git-write seam. Registered by the server at startup; NULL everywhere else
 * (thin client, unit tests), where the git-write tools are simply not offered. */
static agent_git_write_fn g_git_write_fn = NULL;

void agent_tools_set_git_write_provider(agent_git_write_fn fn)
{
   g_git_write_fn = fn;
}

agent_git_write_fn agent_tools_git_write_provider(void)
{
   return g_git_write_fn;
}

int agent_tools_is_git_write(const char *name)
{
   if (!name)
      return 0;
   return strcmp(name, "git_commit") == 0 || strcmp(name, "git_push") == 0 ||
          strcmp(name, "git_branch") == 0 || strcmp(name, "git_pr") == 0;
}

/* The MCP-derived surface: tools declared once in the server's MCP table and
 * registered here so aimee's own agents get them too. See agent_tools.h. */
static agent_mcp_call_fn g_mcp_call_fn = NULL;
static agent_mcp_advert_fn g_mcp_advert_fn = NULL;
static char g_mcp_tools[256][64];
static int g_mcp_tool_count = 0;

void agent_tools_set_mcp_provider(agent_mcp_call_fn call, agent_mcp_advert_fn advert)
{
   g_mcp_call_fn = call;
   g_mcp_advert_fn = advert;
}

agent_mcp_call_fn agent_tools_mcp_call_provider(void)
{
   return g_mcp_call_fn;
}

void agent_tools_register_mcp_tool(const char *name)
{
   if (!name || !name[0])
      return;
   if (agent_tools_is_mcp_derived(name))
      return;
   if (g_mcp_tool_count >= (int)(sizeof(g_mcp_tools) / sizeof(g_mcp_tools[0])))
   {
      LOG_ERROR("agent_tools", "MCP tool registry full (%d); '%s' DROPPED and uncallable",
                g_mcp_tool_count, name);
      return;
   }
   snprintf(g_mcp_tools[g_mcp_tool_count++], sizeof(g_mcp_tools[0]), "%s", name);
}

int agent_tools_is_mcp_derived(const char *name)
{
   if (!name)
      return 0;
   for (int i = 0; i < g_mcp_tool_count; i++)
      if (strcmp(g_mcp_tools[i], name) == 0)
         return 1;
   return 0;
}

/* Advertise the MCP-derived tools, reusing each tool's own MCP schema. A tool whose
 * advert the provider cannot produce is skipped rather than offered with an empty
 * schema: an agent handed a parameterless git_commit will call it wrong. */
static void emit_mcp_tools(cJSON *tools, unsigned surface)
{
   if (!g_mcp_call_fn || !g_mcp_advert_fn)
      return;
   for (int i = 0; i < g_mcp_tool_count; i++)
   {
      cJSON *advert = g_mcp_advert_fn(g_mcp_tools[i]);
      cJSON *schema = cJSON_DetachItemFromObjectCaseSensitive(advert, "inputSchema");
      cJSON *desc = cJSON_GetObjectItemCaseSensitive(advert, "description");
      if (!schema || !cJSON_IsString(desc))
      {
         LOG_WARN("agent_tools", "MCP tool '%s' has no usable advert; not offered natively",
                  g_mcp_tools[i]);
         cJSON_Delete(advert);
         cJSON_Delete(schema);
         continue;
      }
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      if (surface == TSURF_CHAT)
      {
         cJSON *fn = cJSON_CreateObject();
         cJSON_AddStringToObject(fn, "name", g_mcp_tools[i]);
         cJSON_AddStringToObject(fn, "description", desc->valuestring);
         cJSON_AddItemToObject(fn, "parameters", schema);
         cJSON_AddItemToObject(tool, "function", fn);
      }
      else
      {
         cJSON_AddStringToObject(tool, "name", g_mcp_tools[i]);
         cJSON_AddStringToObject(tool, "description", desc->valuestring);
         cJSON_AddItemToObject(tool, "parameters", schema);
      }
      cJSON_AddItemToArray(tools, tool);
      cJSON_Delete(advert);
   }
}

static agent_shell_git_gate_fn g_shell_git_gate = NULL;

void agent_tools_set_shell_git_gate(agent_shell_git_gate_fn fn)
{
   g_shell_git_gate = fn;
}

agent_shell_git_gate_fn agent_tools_shell_git_gate(void)
{
   return g_shell_git_gate;
}

static void emit_builtin_tools(cJSON *tools, unsigned surface)
{
   size_t n = sizeof(g_builtin_tools) / sizeof(g_builtin_tools[0]);
   for (size_t i = 0; i < n; i++)
   {
      const builtin_tool_def_t *d = &g_builtin_tools[i];
      if (!(d->surfaces & surface))
         continue;
      /* Never advertise a tool that cannot run: without the server's git dispatcher
       * the git-write tools have no implementation behind them. Offering them anyway
       * would teach an agent to call a tool that always errors — and, worse, would
       * let require_aimee_git point at tools that do not exist. */
      if (agent_tools_is_git_write(d->name) && !g_git_write_fn)
         continue;
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "function");
      if (surface == TSURF_CHAT)
      {
         cJSON *fn = cJSON_CreateObject();
         cJSON_AddStringToObject(fn, "name", d->name);
         cJSON_AddStringToObject(fn, "description", d->description);
         cJSON_AddItemToObject(fn, "parameters", d->params());
         cJSON_AddItemToObject(tool, "function", fn);
      }
      else
      {
         cJSON_AddStringToObject(tool, "name", d->name);
         cJSON_AddStringToObject(tool, "description", d->description);
         cJSON_AddItemToObject(tool, "parameters", d->params());
      }
      cJSON_AddItemToArray(tools, tool);
   }
}

cJSON *build_tools_array(void)
{
   cJSON *tools = cJSON_CreateArray();
   emit_builtin_tools(tools, TSURF_CHAT);
   emit_mcp_tools(tools, TSURF_CHAT);
   append_remote_tools(tools, 1);
   return tools;
}

/* Build tools array for Responses API (flat format: type, name, description, parameters) */
cJSON *build_tools_array_responses(void)
{
   cJSON *tools = cJSON_CreateArray();
   emit_builtin_tools(tools, TSURF_RESP);
   emit_mcp_tools(tools, TSURF_RESP);
   append_remote_tools(tools, 0);
   return tools;
}

/* Lazy-built schema cache. The cJSON tree is allocated once on the first
 * lookup and intentionally leaked at process exit — small fixed cost,
 * avoids lifetime coordination with every dispatch call site. Mutex guards
 * the first-build race; subsequent reads are lock-free. */
static cJSON *g_schema_cache_tools = NULL;
static pthread_mutex_t g_schema_cache_mu = PTHREAD_MUTEX_INITIALIZER;

cJSON *agent_tool_get_schema_cached(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return NULL;

   if (!g_schema_cache_tools)
   {
      pthread_mutex_lock(&g_schema_cache_mu);
      if (!g_schema_cache_tools)
         g_schema_cache_tools = build_tools_array();
      pthread_mutex_unlock(&g_schema_cache_mu);
   }
   if (!g_schema_cache_tools)
      return NULL;

   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, g_schema_cache_tools)
   {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
      if (!fn)
         continue;
      cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
      if (!cJSON_IsString(name) || strcmp(name->valuestring, tool_name) != 0)
         continue;
      return cJSON_GetObjectItemCaseSensitive(fn, "parameters");
   }
   return NULL;
}

static const char *agent_tools_schema_provider_for_agent(const agent_t *agent)
{
   if (!agent)
      return NULL;
   if (strcmp(agent->provider, "llama_native") == 0 || strcmp(agent->provider, "ollama") == 0 ||
       strcmp(agent->provider, "llama-eval") == 0)
      return agent->provider;
   if (str_contains_ci(agent->name, "llama") || str_contains_ci(agent->model, "qwen") ||
       str_contains_ci(agent->model, ".gguf"))
      return "llama_native";
   return agent->provider;
}

void agent_tools_sanitize_for_provider(cJSON *tools, const char *provider_name)
{
   if (!tools || !cJSON_IsArray(tools) || !provider_name || !provider_name[0])
      return;
   /* Quick gate: only the small/local providers actually need rewrites.
    * Avoids the per-tool deep-copy + delete cycle in the hot path. */
   if (strcmp(provider_name, "llama_native") != 0 && strcmp(provider_name, "llama-eval") != 0 &&
       strcmp(provider_name, "ollama") != 0)
      return;

   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
      if (!fn)
         continue;
      cJSON *params = cJSON_GetObjectItemCaseSensitive(fn, "parameters");
      if (!params)
         continue;
      cJSON *clean = tool_schema_sanitize(provider_name, params);
      if (!clean)
         continue;
      /* Replace in place; tool_schema_sanitize returned a fresh tree, so
       * the swap is safe. */
      cJSON_ReplaceItemInObjectCaseSensitive(fn, "parameters", clean);
   }
}

void agent_tools_sanitize_for_agent(cJSON *tools, const agent_t *agent)
{
   agent_tools_sanitize_for_provider(tools, agent_tools_schema_provider_for_agent(agent));
}

/* Build tools array in Anthropic format by converting from OpenAI format.
 * OpenAI: {"type":"function","function":{"name":"...","description":"...","parameters":{...}}}
 * Anthropic: {"name":"...","description":"...","input_schema":{...}} */
cJSON *build_tools_array_anthropic(void)
{
   cJSON *openai_tools = build_tools_array();
   cJSON *tools = cJSON_CreateArray();

   int n = cJSON_GetArraySize(openai_tools);
   for (int i = 0; i < n; i++)
   {
      cJSON *oi = cJSON_GetArrayItem(openai_tools, i);
      cJSON *fn = cJSON_GetObjectItem(oi, "function");
      if (!fn)
         continue;

      cJSON *tool = cJSON_CreateObject();
      cJSON *name = cJSON_GetObjectItem(fn, "name");
      cJSON *desc = cJSON_GetObjectItem(fn, "description");
      cJSON *params = cJSON_GetObjectItem(fn, "parameters");

      if (name && cJSON_IsString(name))
         cJSON_AddStringToObject(tool, "name", name->valuestring);
      if (desc && cJSON_IsString(desc))
         cJSON_AddStringToObject(tool, "description", desc->valuestring);
      if (params)
         cJSON_AddItemToObject(tool, "input_schema", cJSON_Duplicate(params, 1));

      cJSON_AddItemToArray(tools, tool);
   }

   cJSON_Delete(openai_tools);
   return tools;
}
