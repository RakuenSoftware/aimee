/* Exercise the production shared Git dispatcher, including its early returns. */
#include "aimee.h"
#include "modules/git/mcp_git.h"
#include "modules/git/git_verify.h"
#include "modules/workspace/workspace_turn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char cwd[MAX_PATH_LEN], sid[128], operation_sid[128];
static int overridden, worktree, resolution, mismatch, calls;
static int source_result, mirror_case, resolutions;
static char source_target[MAX_PATH_LEN];
const char *run_cmd_get_cwd(void)
{
   return cwd[0] ? cwd : NULL;
}
void run_cmd_set_cwd(const char *value)
{
   snprintf(cwd, sizeof cwd, "%s", value ? value : "");
}
const char *session_id(void)
{
   return overridden ? sid : "generated-session";
}
int session_id_override_active(void)
{
   return overridden;
}
void session_id_set_override(const char *value)
{
   overridden = 1;
   snprintf(sid, sizeof sid, "%s", value);
}
void session_id_clear_override(void)
{
   overridden = 0;
   sid[0] = 0;
}
void mcp_git_set_worktree(int value)
{
   worktree = value;
}
int mcp_git_get_worktree(void)
{
   return worktree;
}
const char *workspace_turn_git_target(const char *tool, const char *path, const char *directory)
{
   (void)tool;
   return path ? path : directory;
}
int workspace_turn_resolve_mirror_cwd(const char *directory, char *out, size_t capacity)
{
   (void)directory;
   assert(capacity);
   out[0] = 0;
   assert(!"Git must acquire the source rather than resolve an unheld mirror path");
   return 0;
}
static int live_workspace_scopes;
struct workspace_turn_scope
{
   int held;
};
int workspace_turn_bind_active(const char *directory)
{
   (void)directory;
   assert(!"Git must not overwrite the enclosing turn binding");
   return 0;
}
void workspace_turn_unbind_active(void)
{
   assert(!"Git must not release the enclosing turn binding");
}
int workspace_turn_scope_begin(const char *directory, workspace_turn_scope_t **out)
{
   assert(directory && directory[0]);
   snprintf(source_target, sizeof source_target, "%s", directory);
   *out = NULL;
   if (source_result < 0)
      return -1;
   *out = calloc(1, sizeof **out);
   assert(*out);
   live_workspace_scopes++;
   return source_result; /* Even a non-detached scope must be paired. */
}
const char *workspace_turn_active_cwd(void)
{
   assert(live_workspace_scopes == 1);
   return source_result == 1 ? "/retained/mirror" : NULL;
}
int workspace_turn_scope_end(workspace_turn_scope_t *scope)
{
   assert(scope && live_workspace_scopes == 1);
   live_workspace_scopes--;
   free(scope);
   return 0;
}
int mcp_chdir_git_root_authorized(char *old, size_t capacity, cJSON *args, char **error,
                                  int authorized)
{
   (void)old;
   (void)capacity;
   resolutions++;
   assert(live_workspace_scopes == 1);
   if (mirror_case)
   {
      const char *key = mirror_case == 1 ? "path" : "cwd";
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(args, key)),
                     mirror_case == 3 ? "/client/source" : "/retained/mirror"));
      assert(authorized == (mirror_case == 1));
      if (mirror_case == 2)
         assert(cJSON_IsTrue(cJSON_GetObjectItem(args, "no_session_redirect")));
      if (mirror_case == 3)
         assert(cJSON_IsFalse(cJSON_GetObjectItem(args, "no_session_redirect")));
   }
   assert(!strcmp(session_id(), operation_sid));
   run_cmd_set_cwd("/resolved");
   session_id_set_override("resolver-owner");
   worktree = 1;
   *error = mismatch ? strdup("mismatched checkout") : NULL;
   return resolution;
}
static cJSON *git_handler(void)
{
   assert(!strcmp(run_cmd_get_cwd(), "/resolved") && !strcmp(session_id(), "resolver-owner"));
   assert(worktree);
   calls++;
   cJSON *out = cJSON_CreateArray();
   assert(out);
   return out;
}
cJSON *handle_git_verify(server_ctx_t *ctx, cJSON *args, const char *session)
{
   (void)ctx;
   (void)args;
   (void)session;
   assert(!"native Git verify is not dispatched");
   return NULL;
}
int main(void)
{
   for (int variant = 0; variant < 8; variant++)
   {
      for (int outcome = 0; outcome < 7; outcome++)
      {
         char original[128];
         memset(original, 's', sizeof original - 1);
         original[127] = 0;
         int had_override = variant & 1, had_cwd = variant & 2;
         run_cmd_set_cwd(had_cwd ? "/caller/work" : NULL);
         if (had_override)
            session_id_set_override(original);
         else
            session_id_clear_override();
         mcp_git_set_worktree(variant & 1);
         resolution = outcome == 1 ? -1 : outcome == 2 || outcome == 6 ? -2 : outcome == 5 ? 0 : 1;
         mismatch = outcome == 3 || outcome == 4 || outcome == 6;
         calls = 0;
         cJSON *args = cJSON_CreateObject();
         assert(args);
         cJSON_AddStringToObject(args, "cwd", "/requested");
         const char *requested_sid = variant & 4 ? session_id() : "transport";
         snprintf(operation_sid, sizeof operation_sid, "%s", requested_sid);
         cJSON *response =
             mcp_git_run_tool(outcome == 3 ? "git_commit" : "git_status", args, requested_sid);
         assert(response && calls == (outcome == 0 || outcome == 4 || outcome == 5));
         assert(!live_workspace_scopes);
         assert(session_id_override_active() == had_override);
         assert(!strcmp(session_id(), had_override ? original : "generated-session"));
         assert(had_cwd ? run_cmd_get_cwd() && !strcmp(run_cmd_get_cwd(), "/caller/work")
                        : !run_cmd_get_cwd());
         assert(mcp_git_get_worktree() == (variant & 1));
         cJSON_Delete(response);
         cJSON_Delete(args);
      }
   }
   /* Acquisition must see the client identity, while resolution and execution
    * retain the reconstructed source. A reconstruction refusal never probes a
    * server-local fallback or leaves an enclosing binding changed. */
   for (mirror_case = 1; mirror_case <= 3; mirror_case++)
      for (int refused = 0; refused <= 1; refused++)
      {
         source_result = refused ? -1 : mirror_case == 3 ? 2 : 1;
         resolution = 1;
         mismatch = calls = resolutions = 0;
         run_cmd_set_cwd("/outer/source");
         session_id_set_override("outer-session");
         snprintf(operation_sid, sizeof operation_sid, "%s", "mirror-session");
         mcp_git_set_worktree(1);
         cJSON *args = cJSON_CreateObject();
         cJSON_AddStringToObject(args, mirror_case == 1 ? "path" : "cwd", "/client/source");
         cJSON_AddBoolToObject(args, "no_session_redirect", 0);
         cJSON *response = mcp_git_run_tool("git_status", args, "mirror-session");
         assert(response && calls == !refused && resolutions == !refused);
         assert(!strcmp(source_target, "/client/source") && !live_workspace_scopes);
         assert(!strcmp(run_cmd_get_cwd(), "/outer/source"));
         assert(session_id_override_active() && !strcmp(session_id(), "outer-session"));
         assert(mcp_git_get_worktree());
         cJSON_Delete(response);
         cJSON_Delete(args);
      }
   puts("shared Git dispatcher: caller cwd, session and worktree restored on success and refusals");
   return 0;
}

#define GIT_HANDLER(name)                                                                          \
   cJSON *handle_git_##name(cJSON *args)                                                           \
   {                                                                                               \
      (void)args;                                                                                  \
      return git_handler();                                                                        \
   }
GIT_HANDLER(status)
GIT_HANDLER(commit)
GIT_HANDLER(push)
GIT_HANDLER(branch)
GIT_HANDLER(log)
GIT_HANDLER(diff_summary)
GIT_HANDLER(pr)
GIT_HANDLER(pull)
GIT_HANDLER(clone)
GIT_HANDLER(stash)
GIT_HANDLER(tag)
GIT_HANDLER(fetch)
GIT_HANDLER(reset)
GIT_HANDLER(restore)
GIT_HANDLER(issue)
GIT_HANDLER(merge)
GIT_HANDLER(rebase)
GIT_HANDLER(cherry_pick)
GIT_HANDLER(revert)
GIT_HANDLER(sync)
GIT_HANDLER(add)
GIT_HANDLER(switch)
GIT_HANDLER(checkout)
