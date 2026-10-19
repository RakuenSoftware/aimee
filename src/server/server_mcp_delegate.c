/* server_mcp_delegate.c: MCP delegate tool dispatch helpers. */
#include "server_mcp_delegate.h"
#include "aimee.h"
#include "modules/git/mcp_git.h"

#include <stdlib.h>

int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

static int add_resolved_delegate_cwd(cJSON *dreq, cJSON *args, const char *sid, char **error)
{
   char previous_cwd[MAX_PATH_LEN], previous_session[128] = "";
   const char *cwd = run_cmd_get_cwd();
   snprintf(previous_cwd, sizeof previous_cwd, "%s", cwd ? cwd : "");
   int previous_override = session_id_override_active();
   if (previous_override)
      snprintf(previous_session, sizeof previous_session, "%s", session_id());
   int previous_worktree = mcp_git_get_worktree();
   if (sid && sid[0])
      session_id_set_override(sid);
   /* Resolution may change both the cwd and the legacy Git ownership session.
    * Keep those changes inside this lookup; the delegate's parent remains the
    * host-attested transport session, and the caller's tool scope stays intact. */
   char *mismatch = NULL;
   int resolved = mcp_chdir_git_root(NULL, 0, args, &mismatch);
   if (resolved > 0 && !mismatch)
   {
      const char *root = run_cmd_get_cwd();
      if (!root || !root[0] || !cJSON_AddStringToObject(dreq, "cwd", root))
         resolved = -1;
   }
   run_cmd_set_cwd(previous_cwd[0] ? previous_cwd : NULL);
   mcp_git_set_worktree(previous_worktree);
   if (previous_override)
      session_id_set_override(previous_session);
   else
      session_id_clear_override();
   *error = mismatch;
   return resolved < 0 || mismatch ? -1 : 0;
}

int handle_mcp_delegate_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *args, const char *sid)
{
   uint32_t required = server_capability_for_method("delegate");
   if (required && conn && (conn->capabilities & required) == 0)
   {
      server_mcp_served_outcome("refused", "role");
      return server_send_error(conn, "forbidden: insufficient capabilities", NULL);
   }

   cJSON *dreq = cJSON_CreateObject();
   cJSON_AddStringToObject(dreq, "method", "delegate");
   cJSON *jr = cJSON_GetObjectItemCaseSensitive(args, "role");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "prompt");
   cJSON *jb = cJSON_GetObjectItemCaseSensitive(args, "branch");
   cJSON *jdcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
   cJSON *jpersona = cJSON_GetObjectItemCaseSensitive(args, "persona");
   cJSON *jtools = cJSON_GetObjectItemCaseSensitive(args, "tools");
   cJSON *jvia = cJSON_GetObjectItemCaseSensitive(args, "via");
   cJSON *jscope = cJSON_GetObjectItemCaseSensitive(args, "scope");
   cJSON *jhandoff = cJSON_GetObjectItemCaseSensitive(args, "handoff_json");
   /* A persona is required for every delegate (it sets the delegate's identity
    * and principles). */
   if (!cJSON_IsString(jpersona) || !jpersona->valuestring[0])
   {
      server_mcp_served_outcome("error", "bad_args");
      cJSON_Delete(dreq);
      return server_send_error(
          conn, "delegate requires a 'persona' (e.g. engineer, qa, security, reviewer, architect)",
          NULL);
   }
   if (cJSON_IsString(jr))
      cJSON_AddStringToObject(dreq, "role", jr->valuestring);
   if (cJSON_IsString(jp))
      cJSON_AddStringToObject(dreq, "prompt", jp->valuestring);
   if (cJSON_IsString(jb) && jb->valuestring[0])
      cJSON_AddStringToObject(dreq, "branch", jb->valuestring);
   if (cJSON_IsString(jpersona) && jpersona->valuestring[0])
      cJSON_AddStringToObject(dreq, "persona", jpersona->valuestring);
   /* Only a stated boolean is forwarded: absent must stay absent so the role's
    * own tools default decides, exactly as it does for the CLI. */
   if (cJSON_IsBool(jtools))
      cJSON_AddBoolToObject(dreq, "tools", cJSON_IsTrue(jtools));
   if (cJSON_IsString(jvia))
      cJSON_AddStringToObject(dreq, "via", jvia->valuestring);
   if (cJSON_IsString(jscope))
      cJSON_AddStringToObject(dreq, "scope", jscope->valuestring);
   if (cJSON_IsBool(jhandoff))
      cJSON_AddBoolToObject(dreq, "handoff_json", cJSON_IsTrue(jhandoff));
   if (sid && sid[0])
      cJSON_AddStringToObject(dreq, "session_id", sid);
   if (cJSON_IsString(jdcwd) && jdcwd->valuestring[0])
      cJSON_AddStringToObject(dreq, "cwd", jdcwd->valuestring);
   else
   {
      char *error = NULL;
      if (add_resolved_delegate_cwd(dreq, args, sid, &error))
      {
         server_mcp_served_outcome("refused", "context");
         int rc =
             server_send_error(conn, error ? error : "delegate worktree is inaccessible", NULL);
         free(error);
         cJSON_Delete(dreq);
         return rc;
      }
   }
   /* Delegates are always async (WP-B): the in-model tool always returns a
    * job_id and the model polls delegate_status. Set background=1 for
    * back-compat with a mixed-version server during rollout (a current server
    * ignores the field). */
   cJSON_AddBoolToObject(dreq, "background", 1);
   int rc = handle_delegate(ctx, conn, dreq);
   cJSON_Delete(dreq);
   return rc;
}
