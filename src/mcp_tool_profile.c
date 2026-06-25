/* mcp_tool_profile.c: MCP tools/list presentation profile + discovery (P1/P2).
 *
 * Shrinks the initial tools/list shown to an external MCP client. Kept separate
 * from mcp_tools.c (which is at its line budget) and from the tool definitions
 * it filters. See AIMEE_MCP_TOOL_PROFILE; the default is "core" (P2) — lossless
 * because the find_tools/describe_tool discovery meta-tools (also defined here)
 * surface the full catalog on demand. Set it to "full" to present everything. */
#include "cJSON.h"
#include "headers/mcp_tools.h"
#include <stdlib.h>
#include <string.h>

/* Tier-0 "core" presentation profile (MCP-native tool names): the high-frequency
 * tools an external MCP client is shown when AIMEE_MCP_TOOL_PROFILE=core|lean
 * (the default). Everything else — including plugin:* and remote-server tools —
 * is hidden from the initial tools/list to shrink the upfront payload, but stays
 * callable and is reachable via find_tools/describe_tool. Keep this list short
 * and edit it deliberately; it is the floor of what every lean client sees.
 * test_tool_profile_filter mirrors this list and must be kept in sync. */
static const char *const MCP_CORE_TOOLS[] = {
    "get_help",
    "find_tools",    /* discovery: the rest of the catalog is reachable via these */
    "describe_tool", /* discovery */
    "search_docs",   /* orient */
    "search_memory",
    "memory_recall",
    "get_identity", /* grounding */
    "find_symbol",
    "ast_grep_search", /* code intel */
    "git",             /* all git/gh ops via one multiplexed tool (command=...) */
    "delegate",
    "ensemble_review", /* multi-agent */
    "ask_user",
    "send_message", /* interaction */
    "create_note",  /* capture */
    NULL,
};

static int mcp_name_in_set(const char *name, const char *const *set)
{
   for (int i = 0; set[i]; i++)
      if (strcmp(name, set[i]) == 0)
         return 1;
   return 0;
}

const char *mcp_tool_profile_effective(const char *explicit_profile)
{
   if (explicit_profile && explicit_profile[0])
      return explicit_profile;
   const char *e = getenv("AIMEE_MCP_TOOL_PROFILE");
   /* P2 default: "core" — lean is now the out-of-the-box presentation, kept
    * lossless by find_tools/describe_tool. Operators set "full" to opt out. */
   return (e && e[0]) ? e : "core";
}

/* Add the discovery meta-tools to a tools list. These keep the lean default
 * lossless: find_tools surfaces the catalog by keyword, describe_tool returns a
 * tool's full input schema, and the client may then call a tool by name even
 * when it is not in tools/list. Always present (in MCP_CORE_TOOLS above). */
void mcp_add_discovery_tools(cJSON *tools)
{
   if (!tools)
      return;
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "find_tools");
      cJSON_AddStringToObject(
          t, "description",
          "Discover aimee tools beyond the curated core set shown in tools/list. Returns "
          "matching tool names + one-line descriptions (not full schemas). Call "
          "describe_tool(name) for a match's input schema, then call the tool by name. Omit "
          "'query' to list the whole catalog.");
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description",
                              "Case-insensitive keyword matched against tool name + description. "
                              "Omit for the full catalog.");
      cJSON *lim = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(lim, "type", "integer");
      cJSON_AddStringToObject(lim, "description", "Max matches to return (default 50).");
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "describe_tool");
      cJSON_AddStringToObject(t, "description",
                              "Return the full definition (description + input schema) of a single "
                              "tool by name, including tools not shown in tools/list. Pair with "
                              "find_tools to discover names.");
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *nm = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(nm, "type", "string");
      cJSON_AddStringToObject(nm, "description", "Exact tool name (e.g. from find_tools).");
      cJSON *req = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(req, cJSON_CreateString("name"));
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
}

int mcp_filter_tools_for_profile(cJSON *tools, const char *profile)
{
   if (!tools || !cJSON_IsArray(tools))
      return 0;
   profile = mcp_tool_profile_effective(profile);
   /* "full" presents everything; an unknown profile fails OPEN to the full set so
    * a typo never silently hides tools. "core"/"lean" keep only the Tier-0 set. */
   if (strcmp(profile, "core") != 0 && strcmp(profile, "lean") != 0)
      return 0;

   int removed = 0;
   for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--)
   {
      cJSON *tool = cJSON_GetArrayItem(tools, i);
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (!cJSON_IsString(nm) || !mcp_name_in_set(nm->valuestring, MCP_CORE_TOOLS))
      {
         cJSON_DeleteItemFromArray(tools, i);
         removed++;
      }
   }
   return removed;
}
