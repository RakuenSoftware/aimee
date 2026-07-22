/* test_mcp_native_dispatch.c: dispatch_tool_call routing for MCP-derived tools.
 *
 * The other half of the registry merge. test_mcp_native_surface pins the ADVERT
 * (the tool reaches the model's tools array); this pins the DISPATCH (calling that
 * name actually reaches the MCP handler and its output comes back as the plain
 * string the native surface returns).
 *
 * Both halves have failed silently and differently in production:
 *   - git_commit was MCP-only, so a delegate had no route to land work but `git`.
 *   - index_find_callers was advertised-but-unroutable in an early cut of this
 *     merge, and separately advertised from the COLLAPSED tools/list where its flat
 *     name does not exist, so it was dropped from the advert while still resolving
 *     in the toolset.
 * Neither was caught by a test. Both were caught by running a server.
 *
 * The provider is faked: what is under test is the ROUTING (does the name reach the
 * seam, is the fallthrough still "unknown tool", does the content flatten), not any
 * MCP handler. tests/support/agent_dispatch_stubs.c satisfies the other td_*
 * handlers' dependencies at link time — this TU pulls in every tool aimee has, and
 * none of those paths run here. */
#include "aimee.h" /* MAX_PATH_LEN via agent_types.h */
#include "agent_tools.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_TOOL "index_find_callers"

static int g_call_count;
static char g_last_tool[64];
static char g_last_args[256];

static cJSON *fake_advert(const char *tool)
{
   if (!tool || strcmp(tool, FAKE_TOOL) != 0)
      return NULL;
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", FAKE_TOOL);
   cJSON_AddStringToObject(t, "description", "Find the call sites of a symbol.");
   cJSON *s = cJSON_AddObjectToObject(t, "inputSchema");
   cJSON_AddStringToObject(s, "type", "object");
   cJSON_AddObjectToObject(s, "properties");
   return t;
}

/* Returns MCP content blocks, the shape every MCP handler returns. */
static cJSON *fake_call(const char *tool, cJSON *args, const char *sid)
{
   (void)sid;
   g_call_count++;
   snprintf(g_last_tool, sizeof(g_last_tool), "%s", tool ? tool : "");
   cJSON *sym = args ? cJSON_GetObjectItemCaseSensitive(args, "symbol") : NULL;
   snprintf(g_last_args, sizeof(g_last_args), "%s",
            (cJSON_IsString(sym) && sym->valuestring) ? sym->valuestring : "");
   cJSON *content = cJSON_CreateArray();
   cJSON *b1 = cJSON_CreateObject();
   cJSON_AddStringToObject(b1, "type", "text");
   cJSON_AddStringToObject(b1, "text", "toolset.c:137  in toolset_tool_known");
   cJSON_AddItemToArray(content, b1);
   cJSON *b2 = cJSON_CreateObject();
   cJSON_AddStringToObject(b2, "type", "text");
   cJSON_AddStringToObject(b2, "text", "server.c:2158  in server_init");
   cJSON_AddItemToArray(content, b2);
   return content;
}

/* A registered MCP tool's name reaches the provider, with its arguments, and the
 * handler's content blocks come back flattened into the string the native surface
 * hands the model. */
static void test_registered_tool_dispatches_to_the_provider(void)
{
   g_call_count = 0;
   agent_tools_set_mcp_provider(fake_call, fake_advert);
   agent_tools_register_mcp_tool(FAKE_TOOL);

   char *out = dispatch_tool_call(FAKE_TOOL, "{\"symbol\":\"toolset_tool_known\"}", 5000);
   assert(out != NULL);
   assert(g_call_count == 1);
   assert(strcmp(g_last_tool, FAKE_TOOL) == 0);
   /* The arguments survive the trip, not just the name. */
   assert(strcmp(g_last_args, "toolset_tool_known") == 0);
   /* Every text block is flattened, newline-joined — not just the first. */
   assert(strstr(out, "toolset_tool_known") != NULL || strstr(out, "toolset.c:137") != NULL);
   assert(strstr(out, "server.c:2158") != NULL);
   assert(strstr(out, "\n") != NULL);
   free(out);
   printf("  PASS: registered_tool_dispatches_to_the_provider\n");
}

/* An unregistered name must still fall through to the unknown-tool error, not get
 * swallowed by the MCP branch. This is the control that proves the routing is
 * selective rather than a catch-all: on hardware it is what distinguished "the tool
 * ran" from "the tool does not exist". */
static void test_unregistered_name_still_reports_unknown(void)
{
   agent_tools_set_mcp_provider(fake_call, fake_advert);
   char *out = dispatch_tool_call("definitely_not_a_tool", "{}", 5000);
   assert(out != NULL);
   assert(strstr(out, "unknown tool") != NULL);
   free(out);
   printf("  PASS: unregistered_name_still_reports_unknown\n");
}

/* With no provider registered (thin client, unit tests) a registered name must not
 * pretend to work: it is never advertised, so reaching here means a caller invented
 * the name, and the honest answer is an error rather than a crash on a NULL fn. */
static void test_no_provider_is_an_error_not_a_crash(void)
{
   agent_tools_set_mcp_provider(NULL, NULL);
   /* still "registered" from an earlier test — the provider is what is missing */
   char *out = dispatch_tool_call(FAKE_TOOL, "{\"symbol\":\"x\"}", 5000);
   assert(out != NULL);
   assert(strstr(out, "error") != NULL);
   free(out);
   printf("  PASS: no_provider_is_an_error_not_a_crash\n");
}

int main(void)
{
   printf("test_mcp_native_dispatch:\n");
   test_registered_tool_dispatches_to_the_provider();
   test_unregistered_name_still_reports_unknown();
   test_no_provider_is_an_error_not_a_crash();
   printf("All mcp_native_dispatch tests passed.\n");
   return 0;
}
