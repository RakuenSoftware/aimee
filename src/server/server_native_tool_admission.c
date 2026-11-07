/* Coordinate existing tools/directive/execution-policy owners at native MCP
 * ingress. Authorization policy stays with those owners, not this transport. */
#include "server_native_tool_admission.h"
#include "agent_config.h"
#include "agent_coord.h"
#include "agent_exec.h"
#include "request_context.h"
#include "util.h"
#include <aimee/tools/agent_tools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *refuse(char *error, size_t capacity, const char *reason, const char *detail)
{
   if (error && capacity)
      snprintf(error, capacity, "%s", detail);
   return reason;
}

const char *server_native_tool_admit_attempt(const char *tool, const cJSON *arguments,
                                             const char *session, const char *attempt, char *error,
                                             size_t capacity)
{
   if (error && capacity)
      error[0] = 0;
   const request_context_t *owner = request_context_get();
   const char *bound_session = session_id();
   if (!agent_tools_scope_isolated() || !owner || !owner->model_execution || !owner->principal[0] ||
       !owner->request_id[0] || !session || !session[0] || !bound_session ||
       strcmp(session, bound_session) || !attempt || !attempt[0] || strlen(attempt) > 127)
      return refuse(error, capacity, "identity", "native tool execution scope unavailable");
   if (owner->context_refused)
      return refuse(error, capacity, "context", "native tool context was refused");
   if (!tool || !tool[0] || !cJSON_IsObject(arguments))
      return refuse(error, capacity, "bad_args", "native tool requires object arguments");
   if (agent_request_cancelled())
      return refuse(error, capacity, "cancelled", "native tool execution cancelled");

   agent_tool_permission_t permission = agent_tools_permission_check(tool, arguments);
   if (permission != AGENT_TOOL_PERMISSION_ALLOWED)
   {
      char *detail = agent_tools_permission_error(permission);
      const char *reason =
          refuse(error, capacity, permission == AGENT_TOOL_PERMISSION_SHELL ? "permission" : "role",
                 detail ? detail : "native tool permission refused");
      free(detail);
      return reason;
   }
   char *wire = cJSON_PrintUnformatted(arguments);
   if (!wire)
      return refuse(error, capacity, "internal", "cannot serialize native tool arguments");
   char detail[256] = "";
   if (directive_check_tool(tool, wire, detail, sizeof detail) != 0)
   {
      free(wire);
      return refuse(error, capacity, "directive", detail[0] ? detail : "tool directive refused");
   }
   aimee_tool_metadata_t metadata;
   char effect[32];
   if (tool_policy_metadata(tool, &metadata, effect, sizeof effect, detail, sizeof detail) != 0)
   {
      free(wire);
      return refuse(error, capacity, "policy",
                    detail[0] ? detail : "tool policy metadata unavailable");
   }
   int allowed = policy_check_tool_metadata_attempt(tool, &metadata, effect, wire, attempt, detail,
                                                    sizeof detail) == 0;
   free(wire);
   if (!allowed)
      return refuse(error, capacity, "policy",
                    detail[0] ? detail : "tool execution policy refused");
   if (owner->context_refused)
      return refuse(error, capacity, "context", "native tool context was refused");
   if (agent_request_cancelled())
      return refuse(error, capacity, "cancelled", "native tool execution cancelled");
   return NULL;
}

const char *server_native_tool_admit(const char *tool, const cJSON *arguments, const char *session,
                                     char *error, size_t capacity)
{
   const request_context_t *owner = request_context_get();
   char attempt[96] = "";
   if (owner && owner->request_id[0])
      snprintf(attempt, sizeof attempt, "native-mcp:%s", owner->request_id);
   return server_native_tool_admit_attempt(tool, arguments, session, attempt, error, capacity);
}
