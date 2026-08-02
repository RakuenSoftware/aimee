/* mcp_tool_profile.c: MCP tools/list presentation profile + discovery (P1/P2).
 *
 * Shrinks the initial tools/list shown to an external MCP client. Kept separate
 * from mcp_tools.c (which is at its line budget) and from the tool definitions
 * it filters. See AIMEE_MCP_TOOL_PROFILE; the default is "core" (P2) — lossless
 * because the discovery meta-tools plus call_tool bridge (also defined here)
 * surface and dispatch the full catalog on demand. Set it to "full" to present
 * everything. */
#include "cJSON.h"
#include <aimee/protocols/mcp/mcp_tools.h>
#include "agent_code_capabilities.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Tier-0 "core" presentation profile (MCP-native tool names): the high-frequency
 * tools an external MCP client is shown when AIMEE_MCP_TOOL_PROFILE=core|lean
 * (the default). Everything else — including plugin:* and remote-server tools —
 * is hidden from the initial tools/list to shrink the upfront payload, but stays
 * callable through call_tool after find_tools/describe_tool discovery. Keep this
 * list short and edit it deliberately; it is the floor of what every lean client
 * sees.
 * test_tool_profile_filter mirrors this list and must be kept in sync. */
static const char *const MCP_CORE_TOOLS[] = {
    "get_help",
    "find_tools",    /* discovery: the rest of the catalog is reachable via these */
    "describe_tool", /* discovery */
    "call_tool",     /* schema-bound dispatch bridge for discovered tools */
    "search_docs",   /* orient */
    "search_memory",
    "memory_recall",
    "get_identity", /* grounding */
    AIMEE_CODE_TOOL_FIND_SYMBOL,
    AIMEE_CODE_TOOL_AST_GREP_SEARCH,
    AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS, /* direct adoption-critical code intel */
    "git", /* all git/gh ops via one multiplexed tool (command=...) */
    "delegate",
    /* An MCP delegate call returns a job_id and runs in the background, so its
     * poller is not optional: without delegate_status in the floor, an agent
     * that follows our own instruction to delegate cannot read the result
     * without a find_tools -> describe_tool -> call_tool detour. Measured on a
     * real cell, five of fourteen tool calls went on exactly that. This is the
     * same reasoning that already puts roundtable_status here. */
    "delegate_status",
    "roundtable_review", /* multi-agent */
    "roundtable_status", /* poll asynchronous roundtable_review */
    "ask_user",
    "send_message", /* interaction */
    "note",         /* capture (note family: create/list/search) */
    NULL,
};

/* Tools that hand work to a SECOND agent. Their cost, tool calls and edits land
 * outside the caller's transcript, so any measurement of "what did this agent
 * do" stops being attributable the moment one is used. The "solo" profile
 * withholds them; nothing else does. */
static const char *const MCP_MULTI_AGENT_TOOLS[] = {
    "delegate", "delegate_status", "roundtable_review", "roundtable_status", NULL,
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
    * lossless through find_tools/describe_tool + call_tool. Operators set "full"
    * to opt out. */
   return (e && e[0]) ? e : "core";
}

/* Add the discovery meta-tools and dispatch bridge to a tools list. MCP clients
 * generally cannot invent a tool call whose schema was absent from tools/list:
 * find_tools/describe_tool alone therefore make hidden tools discoverable but
 * not callable. call_tool supplies the advertised schema-bound bridge. */
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
          "describe_tool(name) for a match's input schema, then call it through call_tool. "
          "Omit 'query' to list the whole catalog.");
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
   {
      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "name", "call_tool");
      cJSON_AddStringToObject(
          t, "description",
          "Call a tool discovered with find_tools. Pass its exact name and an arguments "
          "object matching the schema returned by describe_tool.");
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *nm = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(nm, "type", "string");
      cJSON_AddStringToObject(nm, "description", "Exact discovered tool name.");
      cJSON *args = cJSON_AddObjectToObject(p, "arguments");
      cJSON_AddStringToObject(args, "type", "object");
      cJSON_AddStringToObject(args, "description",
                              "Arguments matching the discovered tool's input schema; use {} "
                              "for a tool with no parameters.");
      cJSON *req = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(req, cJSON_CreateString("name"));
      cJSON_AddItemToArray(req, cJSON_CreateString("arguments"));
      cJSON_AddItemToObject(t, "inputSchema", s);
      cJSON_AddItemToArray(tools, t);
   }
}

static int mcp_ci_contains(const char *haystack, const char *needle)
{
   if (!needle || !needle[0])
      return 1;
   if (!haystack)
      return 0;
   size_t nlen = strlen(needle);
   for (const char *h = haystack; *h; h++)
      if (strncasecmp(h, needle, nlen) == 0)
         return 1;
   return 0;
}

/* '_' and '-' separate words in tool names; a searcher types spaces. */
static int mcp_query_sep(char c)
{
   return c == ' ' || c == '\t' || c == '_' || c == '-';
}

int mcp_tool_matches_query(const cJSON *tool, const char *query)
{
   if (!tool)
      return 0;
   if (!query || !query[0])
      return 1;

   const cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
   const cJSON *description = cJSON_GetObjectItemCaseSensitive(tool, "description");
   const char *name_s = cJSON_IsString(name) ? name->valuestring : NULL;
   const char *desc_s = cJSON_IsString(description) ? description->valuestring : NULL;

   /* Whole-query match first: preserves phrase searches like "blast radius"
    * hitting a description verbatim. */
   if (mcp_ci_contains(name_s, query) || mcp_ci_contains(desc_s, query))
      return 1;

   /* Otherwise every word of the query must appear somewhere. An agent looking
    * for delegate_status types "delegate status", and a whole-string test finds
    * nothing -- it then dumps the entire catalogue to find one tool. Requiring
    * ALL words keeps this a narrowing search rather than a fuzzy OR. */
   const char *word = query;
   while (*word)
   {
      while (*word && mcp_query_sep(*word))
         word++;
      if (!*word)
         break;
      const char *end = word;
      while (*end && !mcp_query_sep(*end))
         end++;

      size_t len = (size_t)(end - word);
      char token[128];
      if (len == 0 || len >= sizeof(token))
         return 0;
      memcpy(token, word, len);
      token[len] = '\0';

      if (!mcp_ci_contains(name_s, token) && !mcp_ci_contains(desc_s, token))
         return 0;
      word = end;
   }
   return 1;
}

const char *mcp_code_project_from_args(cJSON *args)
{
   const cJSON *project = cJSON_GetObjectItemCaseSensitive(args, "project");
   const char *explicit_project = cJSON_IsString(project) ? project->valuestring : NULL;
   if (explicit_project && explicit_project[0])
      return explicit_project;
   /* The server request boundary resolves cwd to a stable identity before tool
    * dispatch.  A bare basename here would recreate path-keyed project aliases
    * and turn missing context into the wrong project. */
   return NULL;
}

int mcp_code_scope_all(cJSON *args)
{
   cJSON *scope = cJSON_GetObjectItemCaseSensitive(args, "scope");
   if (!scope)
      return 0;
   if (!cJSON_IsString(scope))
      return -1;
   if (!scope->valuestring[0] || strcmp(scope->valuestring, AIMEE_CODE_SCOPE_CURRENT) == 0)
      return 0;
   if (strcmp(scope->valuestring, AIMEE_CODE_SCOPE_ALL) == 0)
      return 1;
   return -1;
}

int mcp_call_tool_demux(const char *tool, cJSON *args, const char **out_tool, cJSON **out_args)
{
   if (!tool || strcmp(tool, "call_tool") != 0)
      return 0;
   if (!cJSON_IsObject(args) || !out_tool || !out_args)
      return -1;

   cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
   cJSON *nested = cJSON_GetObjectItemCaseSensitive(args, "arguments");
   if (!cJSON_IsString(name) || !name->valuestring[0] ||
       strcmp(name->valuestring, "call_tool") == 0 || !cJSON_IsObject(nested))
      return -1;

   *out_tool = name->valuestring;
   *out_args = nested;
   return 1;
}

int mcp_filter_tools_for_profile(cJSON *tools, const char *profile)
{
   if (!tools || !cJSON_IsArray(tools))
      return 0;
   profile = mcp_tool_profile_effective(profile);
   /* "full" presents everything; an unknown profile fails OPEN to the full set so
    * a typo never silently hides tools. "core"/"lean" keep only the Tier-0 set.
    * "solo" is core minus the tools that hand work to another agent -- it must be
    * matched explicitly here, because failing open would grant delegation to a
    * caller that asked for the opposite. */
   int solo = strcmp(profile, "solo") == 0;
   if (!solo && strcmp(profile, "core") != 0 && strcmp(profile, "lean") != 0)
      return 0;

   int removed = 0;
   for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--)
   {
      cJSON *tool = cJSON_GetArrayItem(tools, i);
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(tool, "name");
      int keep = cJSON_IsString(nm) && mcp_name_in_set(nm->valuestring, MCP_CORE_TOOLS) &&
                 !(solo && mcp_name_in_set(nm->valuestring, MCP_MULTI_AGENT_TOOLS));
      if (!keep)
      {
         cJSON_DeleteItemFromArray(tools, i);
         removed++;
      }
   }
   return removed;
}
