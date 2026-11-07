/* server_hooks.c: pre/post-tool hook helpers used by server.c */
#include "aimee.h"
#include "server.h"
#include <sys/stat.h>

/* Optional versioned tool-result interception. Absence leaves ordinary tool
 * execution unchanged; concrete policy and side effects belong to its owner. */
const unsigned aimee_tool_intercept_contract_v1 __attribute__((used, externally_visible, retain)) =
    1;
extern int aimee_module_tool_intercept_result_v1(const aimee_block_t *, const char *, cJSON *,
                                                 const char *, char *, size_t)
    __attribute__((weak));
int server_memory_intercept(const aimee_block_t *call, const char *cwd, cJSON *request,
                            const char *client, char *message, size_t capacity)
{
   if (!aimee_module_tool_intercept_result_v1)
      return 0;
   if (!message || !capacity)
      return 2;
   int result =
       aimee_module_tool_intercept_result_v1(call, cwd, request, client, message, capacity);
   if (result == 0 || result == 2)
      return result;
   memset(message, 0, capacity);
   snprintf(message, capacity, "Tool interception unavailable.");
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
