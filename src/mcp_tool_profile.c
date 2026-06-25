/* mcp_tool_profile.c: MCP tools/list presentation profile (P1).
 *
 * Shrinks the initial tools/list shown to an external MCP client. Kept separate
 * from mcp_tools.c (which is at its line budget) and from the tool definitions
 * it filters. See AIMEE_MCP_TOOL_PROFILE; default "full" is a no-op. */
#include "cJSON.h"
#include "headers/mcp_tools.h"
#include <stdlib.h>
#include <string.h>

/* Tier-0 "core" presentation profile (MCP-native tool names): the high-frequency
 * tools an external MCP client is shown when AIMEE_MCP_TOOL_PROFILE=core|lean.
 * Everything else — including plugin:* and remote-server tools — is hidden from
 * the initial tools/list to shrink the upfront payload. The full catalog stays
 * reachable: P2 adds find_tools/describe_tool for on-demand discovery. Keep this
 * list short and edit it deliberately; it is the floor of what every lean client
 * sees. test_tool_profile_filter mirrors this list and must be kept in sync. */
static const char *const MCP_CORE_TOOLS[] = {
    "get_help",      "search_docs",                                /* orient / discover */
    "search_memory", "memory_recall",     "get_identity",          /* grounding */
    "find_symbol",   "ast_grep_search",                            /* code intel */
    "git_status",    "git_commit",        "git_diff_summary",      /* common git */
    "git_branch",    "git_pr",
    "delegate",      "ensemble_review",                            /* multi-agent */
    "ask_user",      "send_message",                               /* interaction */
    "create_note",                                                 /* capture */
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
   return (e && e[0]) ? e : "full";
}

int mcp_filter_tools_for_profile(cJSON *tools, const char *profile)
{
   if (!tools || !cJSON_IsArray(tools))
      return 0;
   profile = mcp_tool_profile_effective(profile);
   /* "full" (default) presents everything; an unknown profile fails OPEN to the
    * full set so a typo never silently hides tools. */
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
