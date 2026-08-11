/* roundtable_review must demand the original request.
 *
 * The panel's entire job is judging an artifact AGAINST a request. Every scope
 * rule in the seat prompt is stated relative to it -- "adding work the request
 * did not ask for is drift", requirement_coverage, "enumerate what the original
 * request asks for". With original_request optional, a caller could omit it and
 * every one of those rules silently became a no-op: the seat fell back to
 * grading the diff on its own merits, which is generic code review, and generic
 * code review REWARDS thoroughness.
 *
 * Measured on am_270b3483d5: the agent moved trust-bundle CONTENT validation
 * into a preflight the codebase documents as presence-only, rewrote the header
 * comment that documents that layering, and the roundtable approved it twice.
 * Three control arms that made the minimal presence check all passed; the
 * over-validating change was the only failure.
 *
 * Required at the schema boundary so the omission is refused before any tokens
 * are spent, and refused again in the panel (see convene_request_test.go) for
 * callers that do not come through MCP. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* The tool table is built by a static registrar; the suite's convention for
 * those is to include the translation unit. */
#include "aimee/protocols/mcp/mcp_tools.h"

/* The other tool families are irrelevant to this contract and drag in most of
 * the server; stub them so the test links against the schema alone. */
void mcp_add_discovery_tools(cJSON *tools)
{
   (void)tools;
}
void mcp_add_extended_tools(cJSON *tools)
{
   (void)tools;
}
void mcp_add_skill_tools(cJSON *tools)
{
   (void)tools;
}
void mcp_add_gateway_tools(cJSON *tools)
{
   (void)tools;
}
cJSON *session_search_mcp_tool(void)
{
   return NULL;
}
cJSON *mcp_client_registry_build_namespaced_tools(void)
{
   return NULL;
}

static cJSON *schema_for(const char *tool_name)
{
   cJSON *tools = mcp_build_tools_list_flat();
   assert(tools != NULL);
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, tool_name) == 0)
         return cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   }
   return NULL;
}

static int required_has(const cJSON *schema, const char *field)
{
   const cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, req)
   {
      if (cJSON_IsString(item) && strcmp(item->valuestring, field) == 0)
         return 1;
   }
   return 0;
}

int main(void)
{
   printf("mcp_roundtable_contract: ");
   cJSON *schema = schema_for("roundtable_review");
   assert(schema != NULL);

   /* The artifact under review. */
   assert(required_has(schema, "diff"));

   /* The thing it is judged against. Without this the review cannot detect
    * drift, and an approval it could not have earned is worse than no review. */
   assert(required_has(schema, "original_request"));

   /* memory_get must let an agent ask the EVENT-time question.
    *
    * `aimee memory get --as-of <ts>` has answered "was this in force then" since
    * the flag shipped. The TOOL form did not: the schema declared only id/handle
    * and tool_memory_get called the plain kb_client_memory_get, so as_of was
    * unaskable and ignored if passed. The failure mode is the dangerous one --
    * nothing errored, and a memory superseded last week came back looking exactly
    * like a current one. A confident wrong answer, invisible because the row
    * itself was correct. */
   cJSON *mg = schema_for("memory_get");
   assert(mg != NULL);
   cJSON *props = cJSON_GetObjectItemCaseSensitive(mg, "properties");
   assert(props != NULL);
   assert(cJSON_GetObjectItemCaseSensitive(props, "as_of") != NULL);
   assert(cJSON_GetObjectItemCaseSensitive(props, "id") != NULL);     /* additive */
   assert(cJSON_GetObjectItemCaseSensitive(props, "handle") != NULL); /* additive */

   printf("ok\n");
   return 0;
}
