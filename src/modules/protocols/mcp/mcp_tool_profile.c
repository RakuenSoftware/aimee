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
#include <stdio.h> /* snprintf, for the trimmed-description suffix */
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
    /* ast_grep_search is additionally withheld at RUNTIME when no ast-grep
     * binary resolves (server_mcp_surface.c) -- that part stayed, because a tool
     * that cannot run is different from a tool with a cheaper alternative. */
    AIMEE_CODE_TOOL_FIND_SYMBOL,
    AIMEE_CODE_TOOL_AST_GREP_SEARCH,
    AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS,
    AIMEE_CODE_TOOL_INDEX,
    /* SAME REASONING AS delegate_status BELOW, AND THE SAME MEASUREMENT.
     *
     * `index` multiplexes the retrieval an agent needs when the question is not a
     * symbol name: command=hybrid (lexical + dense, bounded by max_results,
     * default 20), plus structure, find_callers, blast_radius and span. Leaving it
     * out of the floor did not make agents use less retrieval -- it made them use
     * a recursive text search instead, because that is one visible call while
     * hybrid cost find_tools -> describe_tool -> call_tool.
     *
     * Measured across the benchmark's aimee cells: 87 shell searches emitting
     * 2.4 MB, 49 of them wide alternations, one large enough to hit the client's
     * 1 MB truncation. index_hybrid answers that class of question with a capped,
     * ranked result set and was reachable the whole time -- just never in one
     * call. A tool the agent cannot afford to reach is a tool it does not have. */
    /* RE-TESTED 2026-08-12 WITH THE COMMAND FORMS IN PLACE, AND THE OLD
     * MEASUREMENT HELD. The argument for removing these four was that the
     * fallback had changed: each is now an `aimee ...` command the standing
     * guidance names, and a command chains where a tool call cannot. That
     * argument was wrong about what the agent actually does.
     *
     * Trimmed vs untrimmed, same task, n=3 each, healthy box:
     *   CLI invocations   2.3 -> 1.3 per cell   (DOWN, not up)
     *   MCP calls         4.3 -> 6.3
     *   shell commands    9.3 -> 11
     *   searches            3 -> 4.3   (4.2 KB -> 6.1 KB of output)
     *   credits         15.52 -> 19.15 mean     (+23%)
     *
     * The MCP calls it did make were find_tools x4, describe_tool x2,
     * call_tool x1 -- the discovery detour the comment above describes, paid in
     * full. Naming a command in guidance does not make the agent prefer it over
     * a schema that is present in every request; removing the schema just sends
     * it to discovery and to grep, exactly as before. */
    "git", /* all git/gh ops via one multiplexed tool (command=...) */
    "delegate",
    /* SAME REASONING AS `index` ABOVE, AND THE SAME MEASUREMENT.
     *
     * The failure that loses the hard tasks is a patch that is reasonable but is
     * not the change that was asked for. roundtable_review exists precisely for
     * that -- `original_request` is documented as goal-drift detection, and every
     * seat is an ordinary delegate, so a one-seat panel is a single reviewer.
     *
     * It was left out of the floor, so reaching it cost find_tools ->
     * describe_tool -> call_tool. Measured on am_b84c9294aa: 74 tool calls, the
     * skill telling the agent to review before reporting done, and roundtable
     * NEVER invoked -- the MCP mix was find_symbol, index, preview_blast_radius,
     * search_memory. The agent shipped the same one-file, 7-line caller-side fix
     * as before, against a reference that changes four files in another module.
     *
     * That looked like guidance being ignored. It was the tool not being visible.
     * A tool the agent cannot afford to reach is a tool it does not have. */
    "roundtable_review", /* blocks and returns the verdict; there is no poller to pair with it */
    /* An MCP delegate call returns a job_id and runs in the background, so its
     * poller is not optional: without delegate_status in the floor, an agent
     * that follows our own instruction to delegate cannot read the result
     * without a find_tools -> describe_tool -> call_tool detour. Measured on a
     * real cell, five of fourteen tool calls went on exactly that. This is the
     * same reasoning that puts roundtable_review here. */
    "delegate_status",
    "roundtable_review", /* multi-agent */
    "ask_user",
    "send_message", /* interaction */
    "note",         /* capture (note family: create/list/search) */
    NULL,
};

/* Floor entries that `index` already multiplexes, dropped only under the "merged"
 * profile. Reachable as `index command=preview`, so dropping the standalone entry
 * removes a duplicate, not a capability.
 *
 * find_symbol and ast_grep_search are NOT here, and adding them to the index family
 * table is not the way to get them here: a family member is folded OUT of the flat
 * tools/list, so that change silently removed both from the default surface for every
 * client (the golden count went 53 -> 51). Multiplexing them is a change to what
 * aimee presents by default, not a presentation-profile tweak, and it needs deciding
 * as such. */
static const char *const MCP_INDEX_MULTIPLEXED_TOOLS[] = {
    AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS,
    NULL,
};

/* The tools that only make sense when delegation runs. delegate_status is here
 * for the reason the floor comment gives for including it: an MCP delegate call
 * returns a job_id and its poller is not optional. With delegation off there are
 * no jobs to poll, so the pair leaves together or not at all. */
static const char *const MCP_DELEGATE_TOOLS[] = {
    "delegate",
    "delegate_status",
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

/* Keep the first sentence (or `cap` bytes), whichever is shorter, and say where the
 * rest went. cap == 0 removes the field outright, which is only ever used for
 * parameter hints -- a tool's own description is never emptied, because a nameless
 * tool is worse than a terse one. */
static void compact_description(cJSON *owner, int cap, int point_at_describe)
{
   cJSON *desc = cJSON_GetObjectItemCaseSensitive(owner, "description");
   if (!cJSON_IsString(desc) || !desc->valuestring)
      return;
   if (cap == 0)
   {
      /* Deleting beats setting "": an empty string still costs the key, the quotes
       * and a comma on every property of every tool, on every turn. */
      cJSON_DeleteItemFromObjectCaseSensitive(owner, "description");
      return;
   }
   const char *s = desc->valuestring;
   int len = (int)strlen(s);
   if (len <= cap)
      return;
   int cut = cap;
   for (int i = 0; i < len - 1 && i < cap; i++)
      if (s[i] == '.' && (s[i + 1] == ' ' || s[i + 1] == '\n'))
      {
         cut = i + 1;
         break;
      }
   char buf[512];
   int n = cut < (int)sizeof(buf) - 32 ? cut : (int)sizeof(buf) - 32;
   memcpy(buf, s, (size_t)n);
   buf[n] = '\0';
   if (point_at_describe)
      snprintf(buf + n, sizeof(buf) - (size_t)n, " (describe_tool for full guidance)");
   cJSON_SetValuestring(desc, buf);
}

int mcp_tool_prose_lean(void)
{
   const char *v = getenv("AIMEE_MCP_TOOL_PROSE");
   return v &&
          (strcasecmp(v, "lean") == 0 || strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
}

int mcp_compact_tool_prose(cJSON *tools)
{
   /* WHAT THIS DOES NOT DO: hide a tool, drop a parameter, or change a type, enum or
    * required list. Every tool stays advertised and callable with exactly the shape it
    * had. Only guidance PROSE is shortened, and describe_tool still serves it in full.
    *
    * Why it is worth doing: tools/list is re-sent as request context on every turn, so
    * its size is a tax paid per turn whether or not a tool is called. Measured on the
    * Ponytail benchmark's aimee arm -- 20,238 bytes over 19 tools, of which 11,894
    * (59%) is prose: 4,053 bytes of top-level descriptions and 7,841 nested in schema
    * property descriptions. `git` alone spends 2,667 of its 4,588 bytes on prose. At
    * ~35 model requests per cell that surface accounted for 47% of the arm's input
    * tokens.
    *
    * The alternative -- presenting fewer tools -- was already tried and is recorded
    * above: agents fell back to recursive shell search rather than pay
    * find_tools -> describe_tool -> call_tool. Trimming prose keeps the whole surface
    * reachable in one call while paying for the part a client actually needs to
    * construct one. */
   if (!tools || !cJSON_IsArray(tools))
      return 0;
   int trimmed = 0;
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      compact_description(tool, MCP_TOOL_PROSE_TOP_CAP, 1);
      cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
      if (!schema)
         schema = cJSON_GetObjectItemCaseSensitive(tool, "input_schema");
      cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
      cJSON *prop = NULL;
      cJSON_ArrayForEach(prop, props)
      {
         /* No describe_tool pointer here: the per-parameter hint is reached through
          * the tool, and repeating the pointer once per property would spend back
          * what the trim saves. */
         compact_description(prop, MCP_TOOL_PROSE_PARAM_CAP, 0);
      }
      trimmed++;
   }
   return trimmed;
}

int mcp_filter_tools_for_profile(cJSON *tools, const char *profile, int delegates_enabled)
{
   if (!tools || !cJSON_IsArray(tools))
      return 0;
   profile = mcp_tool_profile_effective(profile);
   /* "full" presents everything; an unknown profile fails OPEN to the full set so
    * a typo never silently hides tools. "core"/"lean" keep only the Tier-0 set.
    *
    * THERE IS NO "solo" PROFILE. It withheld delegate/roundtable so a run could
    * be measured without a second agent's tokens landing outside the transcript.
    * That makes the thing under measurement a configuration nobody deploys: the
    * benchmark stops describing aimee and starts describing a variant built for
    * the benchmark. If delegates should not run, do not configure them -- that is
    * a real deployment state and it is honest. Hiding shipped tools to flatter a
    * measurement is not. */
   /* "merged" is "core" minus the code-intel tools that `index` now multiplexes:
    * find_symbol -> index command=symbol, ast_grep_search -> command=ast_grep,
    * preview_blast_radius -> command=preview (which already existed -- that entry was
    * a straight duplicate of a subcommand the floor already carried).
    *
    * This is NOT the earlier hide-behind-discovery attempt. That removed the
    * CAPABILITY from the floor, so reaching it cost find_tools -> describe_tool ->
    * call_tool and agents used shell search instead. Here the capability stays in the
    * floor inside `index`, one call away, and its subcommand names ride in the
    * schema's enum -- which survives the prose trim, so a lean client still sees
    * exactly which commands exist. Three definitions (2,483 bytes) become two enum
    * entries. */
   int removed = 0;

   /* Delegation off is a DEPLOYMENT STATE, not a presentation profile -- exactly
    * the distinction the comment above draws when it refuses a "solo" profile.
    * A config that says delegates do not run is a configuration someone actually
    * deploys, so the tools that drive them are genuinely absent and advertising
    * them would be the dishonest half: the agent is told to delegate, calls the
    * tool, and it cannot work.
    *
    * Applied before the profile early-return so it holds for "full" too. A
    * profile decides how much of a working surface to SHOW; this decides what
    * exists. `roundtable_review` is deliberately NOT withheld here -- the review
    * levers change what the prompt DEMANDS, not whether the capability works, and
    * hiding a tool that still functions is the flattery that comment warns
    * against.
    *
    * Taken as a PARAMETER, not read from config here: a module may not reach a
    * peer module directly (check_module_bus_boundary enforces it), and reading
    * config_accessors.h from protocols/ is exactly that crossing. The server owns
    * the config read and passes the answer down. */
   if (!delegates_enabled)
   {
      for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--)
      {
         cJSON *tool = cJSON_GetArrayItem(tools, i);
         cJSON *nm = cJSON_GetObjectItemCaseSensitive(tool, "name");
         if (cJSON_IsString(nm) && mcp_name_in_set(nm->valuestring, MCP_DELEGATE_TOOLS))
         {
            cJSON_DeleteItemFromArray(tools, i);
            removed++;
         }
      }
   }

   int merged = strcmp(profile, "merged") == 0;
   if (strcmp(profile, "core") != 0 && strcmp(profile, "lean") != 0 && !merged)
      return removed;
   for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--)
   {
      cJSON *tool = cJSON_GetArrayItem(tools, i);
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(tool, "name");
      int keep = cJSON_IsString(nm) && mcp_name_in_set(nm->valuestring, MCP_CORE_TOOLS);
      if (keep && merged && mcp_name_in_set(nm->valuestring, MCP_INDEX_MULTIPLEXED_TOOLS))
         keep = 0;
      if (!keep)
      {
         cJSON_DeleteItemFromArray(tools, i);
         removed++;
      }
   }
   return removed;
}
