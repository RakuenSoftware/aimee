/* Shared project preparation for HTTP MCP and the native MCP tool provider. */
#include "server_mcp_internal.h"
#include "aimee.h"
#include "commands.h"

void server_mcp_inject_active_project(cJSON *args, const char *cwd)
{
   if (!cJSON_IsObject(args) || cJSON_GetObjectItemCaseSensitive(args, "project") || !cwd ||
       !cwd[0])
      return;
   char project[MAX_PATH_LEN] = "";
   if (server_active_project_from_cwd(cwd, project, sizeof project) == 0 && project[0])
      cJSON_AddStringToObject(args, "project", project);
}

/* Native calls bypass the HTTP transport preparation. Resolve only from the
 * host-installed cwd, never a model-supplied args.cwd. Preserve the canonical
 * admitted argument object: scope metadata belongs to a disposable call copy. */
cJSON *server_mcp_native_call(const char *tool, cJSON *args, const char *sid)
{
   mcp_tool_handler_fn fn = mcp_tool_lookup(tool);
   if (!fn)
      return NULL;
   if (!cJSON_IsObject(args))
      return text_content("error: native tool arguments must be an object");
   cJSON *prepared = cJSON_Duplicate(args, 1);
   if (!prepared)
      return text_content("error: cannot retain native tool arguments");
   server_mcp_inject_active_project(prepared, run_cmd_get_cwd());
   struct mcp_call c = {
       .ctx = NULL, .conn = NULL, .jargs = prepared, .sid = sid, .tool = tool, .structured = NULL};
   cJSON *result = fn(&c);
   cJSON_Delete(prepared);
   return result;
}
