/* server_mcp_delegate.c: MCP delegate tool dispatch helpers. */
#include "server_mcp_delegate.h"
#include "aimee.h"
#include "mcp_git.h"

#include <stdlib.h>

int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

static void add_resolved_delegate_cwd(cJSON *dreq, cJSON *args, const char *sid)
{
   char resolved_cwd[MAX_PATH_LEN] = "";
   char *mismatch_err = NULL;
   if (sid && sid[0])
      session_id_set_override(sid);
   if (mcp_chdir_git_root(NULL, 0, args, &mismatch_err) > 0)
   {
      const char *tl_cwd = run_cmd_get_cwd();
      if (tl_cwd && tl_cwd[0])
         snprintf(resolved_cwd, sizeof(resolved_cwd), "%s", tl_cwd);
   }
   run_cmd_set_cwd(NULL);
   if (sid && sid[0])
      session_id_clear_override();
   free(mismatch_err);
   if (resolved_cwd[0])
      cJSON_AddStringToObject(dreq, "cwd", resolved_cwd);
}

int handle_mcp_delegate_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *args, const char *sid)
{
   uint32_t required = server_capability_for_method("delegate");
   if (required && conn && (conn->capabilities & required) == 0)
      return server_send_error(conn, "forbidden: insufficient capabilities", NULL);

   cJSON *dreq = cJSON_CreateObject();
   cJSON_AddStringToObject(dreq, "method", "delegate");
   cJSON *jr = cJSON_GetObjectItemCaseSensitive(args, "role");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "prompt");
   cJSON *jb = cJSON_GetObjectItemCaseSensitive(args, "branch");
   cJSON *jdcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
   cJSON *jpersona = cJSON_GetObjectItemCaseSensitive(args, "persona");
   /* A persona is required for every delegate (it sets the delegate's identity
    * and principles). */
   if (!cJSON_IsString(jpersona) || !jpersona->valuestring[0])
   {
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
   if (sid && sid[0])
      cJSON_AddStringToObject(dreq, "session_id", sid);
   if (cJSON_IsString(jdcwd) && jdcwd->valuestring[0])
      cJSON_AddStringToObject(dreq, "cwd", jdcwd->valuestring);
   else
      add_resolved_delegate_cwd(dreq, args, sid);
   /* Delegates are always async (WP-B): the in-model tool always returns a
    * job_id and the model polls delegate_status. Set background=1 for
    * back-compat with a mixed-version server during rollout (a current server
    * ignores the field). */
   cJSON_AddBoolToObject(dreq, "background", 1);
   int rc = handle_delegate(ctx, conn, dreq);
   cJSON_Delete(dreq);
   return rc;
}
