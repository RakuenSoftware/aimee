#ifndef DEC_SESSION_SEARCH_TOOL_H
#define DEC_SESSION_SEARCH_TOOL_H 1

#include "cJSON.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Build the structured result for the zero-LLM session_search MCP tool.
    * The caller owns the returned cJSON object. */
   cJSON *session_search_tool_result_for_principal(const cJSON *args, const char *principal);
   cJSON *session_search_tool_result_for_uid(cJSON *args, unsigned uid);
   cJSON *session_search_mcp_tool(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SESSION_SEARCH_TOOL_H */
