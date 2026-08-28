/* server_hooks.c: pre/post-tool hook helpers used by server.c */
#include "aimee.h"
#include "db1_client/user_memory.h"
#include "harness_memory_audit.h"
#include "harness_memory_common.h"
#include "harness_memory_scope.h"
#include "harness_memory_spill.h"
#include "memory_redirect.h"
#include "server.h"
#include <sys/stat.h>

/* Server-side central agent-memory interception. The split server owns DB1, so
 * it writes the archive row directly; the retired .md is never materialized.
 * Returns 2 (deny, with msg) when intercepted/rejected, else 0. */
int server_memory_intercept(const char *tool, const char *tool_input, const char *cwd, cJSON *req,
                            const char *client, char *msg, size_t msg_len)
{
   const char *home = getenv("HOME");
   if (!client || !client[0] || !home || !home[0] || !hmem_scope_for_client(client))
      return 0;
   cJSON *ti = tool_input ? cJSON_Parse(tool_input) : NULL;
   if (!ti)
      return 0;

   if (strcmp(tool, "Bash") == 0)
   {
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(ti, "command");
      const char *cmd = cJSON_IsString(jc) ? jc->valuestring : NULL;
      int verdict = 0;
      if (cmd && memory_redirect_bash_targets_memory(client, cmd, home))
      {
         snprintf(msg, msg_len,
                  "Memory files are managed by aimee — use the Write tool to set "
                  "memory/<name>.md, not shell redirection.");
         hmem_audit("reject", NULL, NULL, "bash-write-memory");
         verdict = 2;
      }
      cJSON_Delete(ti);
      return verdict;
   }

   cJSON *jp = cJSON_GetObjectItemCaseSensitive(ti, "file_path");
   if (!cJSON_IsString(jp))
      jp = cJSON_GetObjectItemCaseSensitive(ti, "path");
   const char *path = cJSON_IsString(jp) ? jp->valuestring : NULL;
   if (!path)
   {
      cJSON_Delete(ti);
      return 0;
   }

   char name[HMEM_NAME_LEN];
   const char *reason = NULL;
   mr_verdict_t verdict =
       memory_redirect_classify(client, tool, path, home, name, sizeof(name), &reason);
   if (verdict == MR_ALLOW)
   {
      cJSON_Delete(ti);
      return 0;
   }
   if (verdict == MR_REJECT)
   {
      snprintf(msg, msg_len, "%s", reason ? reason : "memory write rejected");
      hmem_audit("reject", NULL, NULL, reason);
      cJSON_Delete(ti);
      return 2;
   }

   cJSON *jcont = cJSON_GetObjectItemCaseSensitive(ti, "content");
   const char *content = cJSON_IsString(jcont) ? jcont->valuestring : NULL;
   if (!content)
   {
      snprintf(msg, msg_len, "Memory Write needs a string 'content' field.");
      cJSON_Delete(ti);
      return 2;
   }

   char project[HMEM_PROJECT_KEY_MAX], rootdir[1024];
   const char *hint =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "harness_project"));
   if (hint && hint[0] && hmem_project_key_ok(hint))
      snprintf(project, sizeof(project), "%s", hint);
   else if (hmem_resolve_project(cwd, project, sizeof(project), rootdir, sizeof(rootdir)) != 0)
   {
      cJSON_Delete(ti);
      return 0;
   }

   char key[HMEM_PROJECT_KEY_MAX + 600];
   snprintf(key, sizeof(key), "archive:%s/%s", project, name);
   if (db1_user_memory_upsert("archive", "L1", key, content, 1.0, client) != 0)
   {
      int sp = hmem_spill_write(project, name, "archive", content);
      hmem_audit(sp == 0 ? "spill" : "spill-failed", project, name, "db1 store unreachable");
      cJSON_Delete(ti);
      return 0;
   }
   hmem_audit("redirect-db1", project, name, NULL);
   snprintf(msg, msg_len,
            "Saved to aimee memory. Memory files are retired — retrieve with "
            "`aimee memory search` and use `aimee memory store` going forward.");
   cJSON_Delete(ti);
   return 2;
}

int hooks_ensure_cwd_worktree(session_state_t *state, const char *sid, const char *cwd)
{
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
