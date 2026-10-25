#ifndef AIMEE_TEST_IR_TOOL_FIXTURE_H
#define AIMEE_TEST_IR_TOOL_FIXTURE_H
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"
#include "tool_args_coerce.h"
#include <string.h>
static inline char *fixture_dispatch_ir_text(const aimee_tool_t *definition, const char *text,
                                             int timeout, int *failed)
{
   cJSON *arguments = text ? aimee_ir_json_parse_frame(text, strlen(text)) : NULL;
   char *result = dispatch_tool_call_ir_ctx_status(definition, arguments, timeout, failed);
   cJSON_Delete(arguments);
   return result;
}
static inline char *fixture_dispatch_file_text(const aimee_tool_t *definition, const char *text,
                                               int timeout, agent_tool_file_resource_fn execute,
                                               void *context, int *failed)
{
   cJSON *arguments = text ? aimee_ir_json_parse_frame(text, strlen(text)) : NULL;
   char *result = dispatch_tool_file_resource_ctx_status(definition, arguments, timeout, execute,
                                                         context, failed);
   cJSON_Delete(arguments);
   return result;
}
/* Text fixture origins for built-in execution tests. The fixture's current
 * schema supplies the same inputs those tests previously authored as strings. */
static inline char *fixture_dispatch_tool_status(const char *name, const char *text, int timeout,
                                                 int *failed)
{
   cJSON *raw = text ? aimee_ir_json_parse_frame(text, strlen(text)) : NULL;
   const cJSON *schema = agent_tool_get_schema_cached(name);
   cJSON *empty = schema ? NULL : cJSON_CreateObject();
   cJSON *arguments = raw ? tool_args_canonicalize(schema, raw) : NULL;
   if (!arguments && cJSON_IsObject(raw))
      arguments = cJSON_Duplicate(raw, 1);
   aimee_tool_t definition = {.kind = AIMEE_TOOL_FUNCTION,
                              .name = (char *)name,
                              .schema = (cJSON *)(schema ? schema : empty)};
   char *result = dispatch_tool_call_ir_ctx_status(&definition, arguments, timeout, failed);
   cJSON_Delete(arguments);
   cJSON_Delete(raw);
   cJSON_Delete(empty);
   return result;
}
static inline char *fixture_dispatch_tool(const char *name, const char *text, int timeout)
{
   return fixture_dispatch_tool_status(name, text, timeout, NULL);
}
#endif
