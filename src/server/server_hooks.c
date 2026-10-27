/* server_hooks.c: pre/post-tool hook helpers used by server.c */
#include "aimee.h"
#include "harness_memory_audit.h"
#include "harness_memory_common.h"
#include "harness_memory_scope.h"
#include "harness_memory_spill.h"
#include "module_stage_adapters.h"
#include "server.h"
#include <sys/stat.h>

typedef enum
{
   MEMORY_REDIRECT_ALLOW = 0,
   MEMORY_REDIRECT_STORE = 1,
   MEMORY_REDIRECT_REJECT = 2
} memory_redirect_verdict_t;

/* Memory-file policy lives in the Go memory module. This is deliberately only
 * the C event-bus edge: copy the hook inputs into JSON and decode the verdict. */
static memory_redirect_verdict_t memory_redirect_bus(const char *operation, const char *client,
                                                     const char *tool, const char *path,
                                                     const char *command, const char *home,
                                                     char *name, size_t name_len, char *reason,
                                                     size_t reason_len)
{
   const hmem_scope_t *surface = hmem_scope_for_client(client);
   if (!surface)
      return MEMORY_REDIRECT_ALLOW;
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return MEMORY_REDIRECT_ALLOW;
   cJSON_AddStringToObject(request, "operation", operation);
   cJSON_AddStringToObject(request, "client", client);
   cJSON_AddStringToObject(request, "home", home ? home : "");
   cJSON_AddStringToObject(request, "projects_root", surface->projects_root);
   cJSON_AddStringToObject(request, "memory_segment", surface->memory_seg);
   if (tool)
      cJSON_AddStringToObject(request, "tool", tool);
   if (path)
      cJSON_AddStringToObject(request, "path", path);
   if (command)
      cJSON_AddStringToObject(request, "command", command);
   cJSON *response = server_module_memory_data(request);
   cJSON_Delete(request);
   if (!response)
      return MEMORY_REDIRECT_ALLOW;
   const char *verdict =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "verdict"));
   const char *reply_name =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "name"));
   const char *reply_reason =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "reason"));
   if (name && name_len)
      snprintf(name, name_len, "%s", reply_name ? reply_name : "");
   if (reason && reason_len)
      snprintf(reason, reason_len, "%s", reply_reason ? reply_reason : "");
   memory_redirect_verdict_t result = MEMORY_REDIRECT_ALLOW;
   if (verdict && strcmp(verdict, "redirect") == 0)
      result = MEMORY_REDIRECT_STORE;
   else if (verdict && strcmp(verdict, "reject") == 0)
      result = MEMORY_REDIRECT_REJECT;
   cJSON_Delete(response);
   return result;
}

/* Server-side central agent-memory interception. Classification and storage
 * both cross the Go memory data stage; the retired .md is never materialized.
 * Returns 2 (deny, with msg) when intercepted/rejected, else 0. */
/* The home a harness memory path is relative to. The path describes the CLIENT's
 * filesystem, so the server's own HOME is right only when both run as the same
 * user on one machine; on CT100 it is /var/lib/aimee, which prefixes no client
 * path, so every memory write was allowed through as a plain file (2026-10-27).
 * Prefer the home the client forwards; for a client that does not forward one,
 * take the part of the path before "/<projects_root>/". Only a trusted hook
 * reaches here, and the value decides nothing but whether this caller's own
 * write is classified as its memory file. */
static int safe_home(const char *home, size_t len)
{
   if (!home || len == 0 || len >= 1024 || home[0] != '/')
      return 0;
   for (size_t i = 0; i + 1 < len; i++)
      if (home[i] == '/' && home[i + 1] == '.')
         return 0; /* no ".", ".." or hidden segment in a home */
   return 1;
}

static const char *memory_intercept_home(const cJSON *req, const char *client, const char *path,
                                         char *buf, size_t buf_len)
{
   const char *forwarded =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "harness_home"));
   if (forwarded && safe_home(forwarded, strlen(forwarded)))
      return forwarded;
   const hmem_scope_t *surface = hmem_scope_for_client(client);
   if (path && surface && surface->projects_root && surface->projects_root[0])
   {
      char marker[512];
      snprintf(marker, sizeof marker, "/%s/", surface->projects_root);
      const char *at = strstr(path, marker);
      if (at && safe_home(path, (size_t)(at - path)) && (size_t)(at - path) < buf_len)
      {
         memcpy(buf, path, (size_t)(at - path));
         buf[at - path] = '\0';
         return buf;
      }
   }
   return getenv("HOME");
}

int server_memory_intercept(const aimee_block_t *call, const char *cwd, cJSON *req,
                            const char *client, char *msg, size_t msg_len)
{
   if (!call || call->type != AIMEE_BLK_TOOL_USE || !call->tool_name ||
       (call->tool_namespace && call->tool_namespace[0]) || !client || !client[0] ||
       !hmem_scope_for_client(client))
      return 0;
   const char *tool = call->tool_name;
   const cJSON *ti = call->tool_input;
   if (!cJSON_IsObject(ti))
      return 0;
   char derived_home[1024];
   cJSON *jpath = cJSON_GetObjectItemCaseSensitive(ti, "file_path");
   if (!cJSON_IsString(jpath))
      jpath = cJSON_GetObjectItemCaseSensitive(ti, "path");
   const char *home =
       memory_intercept_home(req, client, cJSON_IsString(jpath) ? jpath->valuestring : NULL,
                             derived_home, sizeof derived_home);
   if (!home || !home[0])
      return 0;

   if (strcmp(tool, "Bash") == 0)
   {
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(ti, "command");
      const char *cmd = cJSON_IsString(jc) ? jc->valuestring : NULL;
      int verdict = 0;
      if (cmd && memory_redirect_bus("redirect-bash", client, tool, NULL, cmd, home, NULL, 0, NULL,
                                     0) == MEMORY_REDIRECT_REJECT)
      {
         snprintf(msg, msg_len,
                  "Memory files are managed by aimee — use the Write tool to set "
                  "memory/<name>.md, not shell redirection.");
         hmem_audit("reject", NULL, NULL, "bash-write-memory");
         verdict = 2;
      }
      return verdict;
   }

   cJSON *jp = cJSON_GetObjectItemCaseSensitive(ti, "file_path");
   if (!cJSON_IsString(jp))
      jp = cJSON_GetObjectItemCaseSensitive(ti, "path");
   const char *path = cJSON_IsString(jp) ? jp->valuestring : NULL;
   if (!path)
   {
      return 0;
   }

   char name[HMEM_NAME_LEN];
   char reason[512] = "";
   memory_redirect_verdict_t verdict =
       memory_redirect_bus("redirect-classify", client, tool, path, NULL, home, name, sizeof(name),
                           reason, sizeof(reason));
   if (verdict == MEMORY_REDIRECT_ALLOW)
   {
      return 0;
   }
   if (verdict == MEMORY_REDIRECT_REJECT)
   {
      snprintf(msg, msg_len, "%s", reason[0] ? reason : "memory write rejected");
      hmem_audit("reject", NULL, NULL, reason[0] ? reason : NULL);
      return 2;
   }

   cJSON *jcont = cJSON_GetObjectItemCaseSensitive(ti, "content");
   const char *content = cJSON_IsString(jcont) ? jcont->valuestring : NULL;
   if (!content)
   {
      snprintf(msg, msg_len, "Memory Write needs a string 'content' field.");
      return 2;
   }

   char project[HMEM_PROJECT_KEY_MAX], rootdir[1024];
   const char *hint =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "harness_project"));
   if (hint && hint[0] && hmem_project_key_ok(hint))
      snprintf(project, sizeof(project), "%s", hint);
   else if (hmem_resolve_project(cwd, project, sizeof(project), rootdir, sizeof(rootdir)) != 0)
   {
      return 0;
   }

   char key[HMEM_PROJECT_KEY_MAX + 600];
   snprintf(key, sizeof(key), "archive:%s/%s", project, name);
   cJSON *store = cJSON_CreateObject();
   if (store)
   {
      cJSON_AddStringToObject(store, "operation", "store");
      cJSON_AddStringToObject(store, "kind", "archive");
      cJSON_AddStringToObject(store, "tier", "L1");
      cJSON_AddStringToObject(store, "key", key);
      cJSON_AddStringToObject(store, "content", content);
      cJSON_AddNumberToObject(store, "confidence", 1.0);
   }
   cJSON *stored = store ? server_module_memory_data(store) : NULL;
   cJSON_Delete(store);
   if (!stored)
   {
      int sp = hmem_spill_write(project, name, "archive", content);
      hmem_audit(sp == 0 ? "spill" : "spill-failed", project, name, "db1 store unreachable");
      return 0;
   }
   cJSON_Delete(stored);
   hmem_audit("redirect-db1", project, name, NULL);
   snprintf(msg, msg_len,
            "Saved to aimee memory. Memory files are retired — retrieve with "
            "`aimee memory search` and use `aimee memory store` going forward.");
   return 2;
}

int hooks_ensure_cwd_worktree(session_state_t *state, const char *sid, const char *cwd)
{
   if (!config_require_session_worktree())
      return 0;

   if (!state || !sid || !sid[0] || strcmp(sid, "unknown") == 0 || !cwd || !cwd[0])
      return 0;

   char git_root[MAX_PATH_LEN];
   if (git_repo_root(cwd, git_root, sizeof(git_root)) != 0 || !git_root[0])
      return 0;

   char expected[MAX_PATH_LEN];
   if (worktree_sibling_path(git_root, sid, NULL, expected, sizeof(expected)) != 0 || !expected[0])
      return 0;

   struct stat st;
   if (stat(expected, &st) != 0 || !S_ISDIR(st.st_mode))
      (void)worktree_create_sibling(git_root, sid, NULL);

   for (int i = 0; i < state->worktree_count; i++)
   {
      if (strcmp(state->worktrees[i].git_root, git_root) == 0)
      {
         if (strcmp(state->worktrees[i].worktree_path, expected) != 0)
         {
            snprintf(state->worktrees[i].worktree_path, sizeof(state->worktrees[i].worktree_path),
                     "%s", expected);
            state->dirty = 1;
            return 1;
         }
         return 0;
      }
   }

   if (state->worktree_count >= MAX_WORKTREES)
      return 0;

   worktree_mapping_t *m = &state->worktrees[state->worktree_count++];
   snprintf(m->git_root, sizeof(m->git_root), "%s", git_root);
   snprintf(m->worktree_path, sizeof(m->worktree_path), "%s", expected);
   state->dirty = 1;
   return 1;
}
