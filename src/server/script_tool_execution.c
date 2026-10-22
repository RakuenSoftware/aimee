/* Script transport ingress -> canonical tool definition and supervised policy. */
#include "script_tool_execution.h"
#include "aimee.h"
#include "agent_exec.h"
#include "agent_coord.h"
#include "agent_config.h"
#include "request_context.h"
#include "tool_args_coerce.h"
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/aimee_ir.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *server_script_tool_execute(const cJSON *frame, const char *attempt, int *is_error)
{
   if (is_error)
      *is_error = 1;
   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "tool"));
   const cJSON *raw = cJSON_GetObjectItemCaseSensitive(frame, "args");
   const request_context_t *owner = request_context_get();
   if (!aimee_ir_json_unique(frame) || !name || !name[0] || !cJSON_IsObject(raw))
      return safe_strdup("error: invalid script tool frame");
   if (!agent_tools_scope_isolated() || !owner || !owner->model_execution || !owner->principal[0] ||
       owner->context_refused || !attempt || !attempt[0])
      return safe_strdup("error: script tool execution scope unavailable");
   if (agent_request_cancelled())
      return safe_strdup("error: script tool execution cancelled");
   aimee_tool_t definition = {0};
   if (agent_tools_execution_definition(name, &definition) != 0)
      return safe_strdup("error: script tool has no current canonical definition");
   cJSON *arguments = tool_args_canonicalize(definition.schema, raw);
   char *wire = arguments ? cJSON_PrintUnformatted(arguments) : NULL;
   char detail[256] = "", effect[32];
   aimee_tool_metadata_t metadata;
   char *result = NULL;
   agent_tool_permission_t permission =
       arguments ? agent_tools_permission_check(name, arguments) : AGENT_TOOL_PERMISSION_ROLE;
   if (!wire)
      result = safe_strdup("error: cannot admit script tool arguments");
   else if (tool_validate_ir(&definition, wire, detail, sizeof detail) != 0)
      result = safe_strdup(detail);
   else if (permission != AGENT_TOOL_PERMISSION_ALLOWED)
      result = agent_tools_permission_error(permission);
   else if (directive_check_tool(name, wire, detail, sizeof detail) != 0 ||
            tool_policy_metadata(name, &metadata, effect, sizeof effect, detail, sizeof detail) !=
                0 ||
            policy_check_tool_metadata_attempt(name, &metadata, effect, wire, attempt, detail,
                                               sizeof detail) != 0)
      result = safe_strdup(detail);
   else if (owner->context_refused || agent_request_cancelled())
      result = safe_strdup("error: script tool execution cancelled or context refused");
   else
   {
      agent_tools_set_effect_authorized(1);
      result = dispatch_tool_call_ir_ctx_status(&definition, wire, 120000, is_error);
      agent_tools_set_effect_authorized(0);
      result = policy_annotate_indexed(name, wire, attempt, result);
   }
   cJSON_Delete(arguments);
   free(wire);
   aimee_tool_free(&definition);
   if (!result && is_error)
      *is_error = 1;
   return result;
}
