/* mcp_tools.c: shared MCP tool definitions */
#include "cJSON.h"
#include "headers/mcp_client_registry.h"
#include "headers/mcp_skill_tools.h"
#include "headers/mcp_tools.h"
#include "headers/mcp_tools_gateway.h"
#include "headers/plugin.h"
#include "headers/session_search_tool.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
static cJSON *build_tool(const char *name, const char *desc, cJSON *schema)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON_AddItemToObject(t, "inputSchema", schema);
   return t;
}
static int tools_array_has_name(cJSON *tools, const char *name)
{
   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (cJSON_IsString(tool_name) && strcmp(tool_name->valuestring, name) == 0)
         return 1;
   }
   return 0;
}
static void add_session_context_tools(cJSON *tools)
{
   cJSON_AddItemToArray(tools, session_search_mcp_tool());
   cJSON_AddItemToArray(
       tools, build_tool("session_context_search",
                         "Search tool-chain stubs from the current session. Returns compacted "
                         "summaries matching the query. Requires virtual_context.enabled=true.",
                         cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                                     "\"query\":{\"type\":\"string\","
                                     "\"description\":\"Substring to search over stubs\"},"
                                     "\"limit\":{\"type\":\"integer\","
                                     "\"description\":\"Maximum results (default 10)\"}},"
                                     "\"required\":[\"query\"]}")));
   cJSON_AddItemToArray(
       tools, build_tool("session_context_expand",
                         "Expand a tool-chain stub to full raw tool events. Use after "
                         "session_context_search. Requires virtual_context.enabled=true.",
                         cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                                     "\"chain_id\":{\"type\":\"integer\","
                                     "\"description\":\"Chain id from session_context_search\"}},"
                                     "\"required\":[\"chain_id\"]}")));
   cJSON_AddItemToArray(
       tools, build_tool("session_context_status",
                         "Virtual context assembly status: enabled flag, event/chain counts.",
                         cJSON_Parse("{\"type\":\"object\",\"properties\":{}}")));
   cJSON_AddItemToArray(
       tools, build_tool("payload_rewrite_status",
                         "Prompt-cache-aware rewrite status: enabled flag, payload epoch, "
                         "deferred/forced counts, bytes saved pending forced rewrite.",
                         cJSON_Parse("{\"type\":\"object\",\"properties\":{}}")));
   mcp_add_gateway_tools(tools);
}
cJSON *mcp_build_tools_list(void)
{
   cJSON *tools = cJSON_CreateArray();
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *t = cJSON_AddObjectToObject(p, "topic");
      cJSON_AddStringToObject(t, "type", "string");
      cJSON_AddStringToObject(t, "description",
                              "Topic to look up (e.g. 'work queue', 'delegate', 'git'). "
                              "Omit to get the topic index.");
      cJSON_AddItemToArray(
          tools,
          build_tool("get_help",
                     "CALL THIS TOOL FIRST at the start of every session before doing any work. "
                     "With no args, returns a compact topic index. Pass a topic name to get "
                     "details for that section only (e.g. 'work queue', 'mcp tools', "
                     "'delegate', 'memory', 'build', 'conventions').",
                     s));
   }
   mcp_add_skill_tools(tools);
   /* search_memory */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Search terms to find matching facts and memories");
      cJSON *f = cJSON_AddObjectToObject(p, "filter");
      cJSON_AddStringToObject(f, "type", "object");
      cJSON_AddStringToObject(f, "description",
                              "Canonical scope/filter: {scope:{workspace,project,session},"
                              "filters:{tier[],kind[],entity[]}}. Omit to search all scopes.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("search_memory",
                            "Search aimee's knowledge base for stored facts and memories. "
                            "Returns matching L2/L3 facts by keyword.",
                            s));
   }

   /* search_graph */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description",
                              "Search terms to find matching graph relations and episodes");
      cJSON *l = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(l, "type", "integer");
      cJSON_AddStringToObject(l, "description", "Maximum number of graph relations to return");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("search_graph",
                     "Search aimee's episode and relationship graph for matching evidence.", s));
   }

   /* get_episode */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "episode_key");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Episode key or session identifier to fetch");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("episode_key"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("get_episode", "Fetch a memory episode by key or source session.", s));
   }

   /* get_entity */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "entity");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Entity name to inspect in aimee memory");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("entity"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("get_entity",
                     "Fetch a memory-backed entity profile with counts and summary evidence.", s));
   }

   /* get_entity_edges */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "entity");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Entity name whose graph edges should be listed");
      cJSON *l = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(l, "type", "integer");
      cJSON_AddStringToObject(l, "description", "Maximum number of edges to return");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("entity"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("get_entity_edges",
                                             "Fetch graph relations connected to an entity.", s));
   }

   /* get_context_block */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Query to assemble context for");
      cJSON *b = cJSON_AddObjectToObject(p, "block_type");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(
          b, "description", "Context block type: general, timeline, episode, entity, relationship");
      cJSON *l = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(l, "type", "integer");
      cJSON_AddStringToObject(l, "description", "Maximum items to include in the block");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("get_context_block",
                            "Assemble a deterministic memory context block for a query.", s));
   }

   /* memory_get */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *id = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(id, "type", "integer");
      cJSON_AddStringToObject(id, "description", "Memory row id to fetch");
      cJSON *h = cJSON_AddObjectToObject(p, "handle");
      cJSON_AddStringToObject(h, "type", "string");
      cJSON_AddStringToObject(h, "description", "Handle emitted in previews, e.g. memory:123");
      cJSON_AddItemToArray(
          tools, build_tool("memory_get", "Fetch a full memory by id or memory:<id> handle.", s));
   }

   /* list_facts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("list_facts",
                            "List all stored facts in aimee's long-term memory (L2 tier).", s));
   }

   /* memory_briefing */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *l = cJSON_AddObjectToObject(p, "limit_tokens");
      cJSON_AddStringToObject(l, "type", "integer");
      cJSON_AddStringToObject(l, "description",
                              "Approximate character/token budget for the returned bundle "
                              "(default 1500). Lower sections are truncated first.");
      cJSON_AddItemToArray(
          tools, build_tool("memory_briefing",
                            "Return a deterministic start-of-session context bundle: "
                            "top key facts, recent session activity, and active entities. "
                            "Pure DB queries, no LLM calls.",
                            s));
   }

   /* get_identity */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("get_identity",
                            "Return the operator-authored charter (immutable) and the learned "
                            "working-profile state (committed-only) as structured JSON. Useful "
                            "when the agent is asked to explain its own constraints or "
                            "preferences.",
                            s));
   }

   /* list_curiosity_items */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *st = cJSON_AddObjectToObject(p, "state");
      cJSON_AddStringToObject(st, "type", "string");
      cJSON_AddStringToObject(
          st, "description",
          "Filter by state: open (default), in_progress, resolved, suppressed, or empty for all.");
      cJSON *lim = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(lim, "type", "integer");
      cJSON_AddStringToObject(lim, "description", "Maximum rows to return (default 20).");
      cJSON_AddItemToArray(
          tools, build_tool("list_curiosity_items",
                            "Return the durable curiosity backlog — knowledge gaps, "
                            "contradictions, stale facts, weak-coverage entities, and unverified "
                            "assumptions. Inspection-only; no scoring or action routing yet.",
                            s));
   }

   /* create_prospective_memory */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *tr = cJSON_AddObjectToObject(p, "trigger_text");
      cJSON_AddStringToObject(tr, "type", "string");
      cJSON_AddStringToObject(tr, "description",
                              "Free-text description of when this reminder should fire "
                              "(e.g. \"when CI comes up\").");
      cJSON *ac = cJSON_AddObjectToObject(p, "action_text");
      cJSON_AddStringToObject(ac, "type", "string");
      cJSON_AddStringToObject(ac, "description",
                              "The reminder itself — what to surface when the trigger fires.");
      cJSON *ae = cJSON_AddObjectToObject(p, "anchor_entity");
      cJSON_AddStringToObject(ae, "type", "string");
      cJSON_AddStringToObject(ae, "description",
                              "Optional exact-match entity anchor (e.g. a proper noun).");
      cJSON *af = cJSON_AddObjectToObject(p, "anchor_file");
      cJSON_AddStringToObject(af, "type", "string");
      cJSON_AddStringToObject(af, "description", "Optional exact-match file path anchor.");
      cJSON *re = cJSON_AddObjectToObject(p, "recurrence");
      cJSON_AddStringToObject(re, "type", "string");
      cJSON_AddStringToObject(re, "description", "\"once\" (default) or \"repeat\".");
      cJSON *vu = cJSON_AddObjectToObject(p, "valid_until");
      cJSON_AddStringToObject(vu, "type", "string");
      cJSON_AddStringToObject(vu, "description",
                              "Optional ISO-8601 timestamp; empty means never expires.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("trigger_text"));
      cJSON_AddItemToArray(req, cJSON_CreateString("action_text"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("create_prospective_memory",
                            "Arm a prospective memory (\"when X, surface Y\") that the pre-turn "
                            "matcher will fire on future sessions until completed or expired.",
                            s));
   }

   /* list_prospective_memories */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *st = cJSON_AddObjectToObject(p, "state");
      cJSON_AddStringToObject(st, "type", "string");
      cJSON_AddStringToObject(st, "description",
                              "Filter by state: armed, triggered, completed, expired. "
                              "Empty = all.");
      cJSON *l = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(l, "type", "integer");
      cJSON_AddStringToObject(l, "description", "Max rows to return (default 50, cap 256).");
      cJSON_AddItemToArray(tools,
                           build_tool("list_prospective_memories",
                                      "List prospective memories (reminders) ordered newest-first. "
                                      "Optionally filter by state.",
                                      s));
   }

   /* complete_prospective_memory */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *i = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(i, "type", "integer");
      cJSON_AddStringToObject(i, "description", "The reminder id to complete.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("complete_prospective_memory",
                            "Mark a prospective memory as completed so it no longer surfaces.", s));
   }

   /* memory_alerts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *since = cJSON_AddObjectToObject(p, "since");
      cJSON_AddStringToObject(since, "type", "string");
      cJSON_AddStringToObject(since, "description",
                              "Optional ISO-8601 timestamp — bound the newly-superseded "
                              "section. Empty means \"last 7 days\".");
      cJSON_AddItemToArray(
          tools, build_tool("memory_alerts",
                            "Surface stale pending commitments, unresolved contradictions, "
                            "and newly superseded memories as a single bundle so the agent "
                            "can act on issues without having to think to ask.",
                            s));
   }

   /* memory_recall */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *task = cJSON_AddObjectToObject(p, "task_hint");
      cJSON_AddStringToObject(task, "type", "string");
      cJSON_AddStringToObject(task, "description",
                              "Optional current-turn user text. Empty for session-start recall.");
      cJSON *ss = cJSON_AddObjectToObject(p, "session_start");
      cJSON_AddStringToObject(ss, "type", "boolean");
      cJSON_AddStringToObject(ss, "description",
                              "If true, assemble a larger session-start bundle (higher per-"
                              "section caps).");
      cJSON *lt = cJSON_AddObjectToObject(p, "limit_tokens");
      cJSON_AddStringToObject(lt, "type", "integer");
      cJSON_AddStringToObject(lt, "description",
                              "Character/token budget for the rendered bundle. 0 = default "
                              "(1800 session, 600 per-turn).");
      cJSON_AddItemToArray(tools,
                           build_tool("memory_recall",
                                      "Return a six-section proactive-recall bundle (identity, "
                                      "preferences, active_context, open_commitments, reminders, "
                                      "directives) suitable for prompt injection. Pure DB queries.",
                                      s));
   }

   /* list_epistemic_directives */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *state = cJSON_AddObjectToObject(p, "state");
      cJSON_AddStringToObject(state, "type", "string");
      cJSON_AddStringToObject(state, "description",
                              "Filter by state: open|suppressed|resolved|expired. "
                              "Omit for all states.");
      cJSON *cause = cJSON_AddObjectToObject(p, "cause");
      cJSON_AddStringToObject(cause, "type", "string");
      cJSON_AddStringToObject(cause, "description",
                              "Filter by cause: contradiction|retrieval_failure|"
                              "missing_config|user_follow_up.");
      cJSON *limit = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(limit, "type", "integer");
      cJSON_AddStringToObject(limit, "description", "Max rows to return (default 50, max 256).");
      cJSON_AddItemToArray(
          tools, build_tool("list_epistemic_directives",
                            "List epistemic directives ordered by priority DESC.  Durable "
                            "\"ask the user when relevant\" records auto-created from "
                            "contradictions and retrieval failures.",
                            s));
   }

   /* create_epistemic_directive */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "question");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "The question to ask the user when relevant.");
      cJSON *tp = cJSON_AddObjectToObject(p, "topic");
      cJSON_AddStringToObject(tp, "type", "string");
      cJSON *en = cJSON_AddObjectToObject(p, "anchor_entity");
      cJSON_AddStringToObject(en, "type", "string");
      cJSON *fi = cJSON_AddObjectToObject(p, "anchor_file");
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON *cs = cJSON_AddObjectToObject(p, "cause");
      cJSON_AddStringToObject(cs, "type", "string");
      cJSON_AddStringToObject(cs, "description",
                              "contradiction|retrieval_failure|missing_config|user_follow_up "
                              "(defaults to user_follow_up).");
      cJSON *pr = cJSON_AddObjectToObject(p, "priority");
      cJSON_AddStringToObject(pr, "type", "integer");
      cJSON_AddStringToObject(pr, "description", "0-100, higher wins. Default 50.");
      cJSON *vu = cJSON_AddObjectToObject(p, "valid_until");
      cJSON_AddStringToObject(vu, "type", "string");
      cJSON_AddStringToObject(vu, "description", "ISO-8601 expiry. Empty = never expires.");
      cJSON *req = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(req, cJSON_CreateString("question"));
      cJSON_AddItemToArray(tools, build_tool("create_epistemic_directive",
                                             "Create a new open directive. Dedup-safe via unique "
                                             "indexes on (cause, topic) and (cause, memory pair).",
                                             s));
   }

   /* resolve_epistemic_directive */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *id = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(id, "type", "integer");
      cJSON *rm = cJSON_AddObjectToObject(p, "resolution_memory_id");
      cJSON_AddStringToObject(rm, "type", "integer");
      cJSON_AddStringToObject(rm, "description",
                              "Optional memory id whose content answers "
                              "the question.");
      cJSON *note = cJSON_AddObjectToObject(p, "note");
      cJSON_AddStringToObject(note, "type", "string");
      cJSON *suppress = cJSON_AddObjectToObject(p, "suppress");
      cJSON_AddStringToObject(suppress, "type", "boolean");
      cJSON_AddStringToObject(suppress, "description", "If true, suppress instead of resolve.");
      cJSON *req = cJSON_AddArrayToObject(s, "required");
      cJSON_AddItemToArray(req, cJSON_CreateString("id"));
      cJSON_AddItemToArray(tools, build_tool("resolve_epistemic_directive",
                                             "Transition an open directive to resolved (with "
                                             "optional linking memory) or suppressed.",
                                             s));
   }

   /* memory_maintain */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *m = cJSON_AddObjectToObject(p, "modes");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description",
                              "Comma-separated subset of replay|compact|prune|summarize. "
                              "Empty = replay,compact,prune (default).");
      cJSON *dr = cJSON_AddObjectToObject(p, "dry_run");
      cJSON_AddStringToObject(dr, "type", "boolean");
      cJSON_AddStringToObject(dr, "description",
                              "If true, surface what would change without mutating.");
      cJSON *fc = cJSON_AddObjectToObject(p, "force");
      cJSON_AddStringToObject(fc, "type", "boolean");
      cJSON_AddStringToObject(fc, "description", "Bypass the idle-guard so the cycle always runs.");
      cJSON_AddItemToArray(
          tools, build_tool("memory_maintain",
                            "Run a memory maintenance cycle (replay/compact/prune/summarize) and "
                            "return the summary. Idempotent; idle cycles short-circuit.",
                            s));
   }

   /* get_host */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Hostname to look up (e.g. 'proxmox', 'wol-web')");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("name"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("get_host",
                                      "Look up a specific host by name from the network inventory. "
                                      "Returns IP, port, user, and description.",
                                      s));
   }

   /* list_hosts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("list_hosts",
                            "List all hosts and networks in the infrastructure inventory.", s));
   }

   /* find_symbol */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *id = cJSON_AddObjectToObject(p, "identifier");
      cJSON_AddStringToObject(id, "type", "string");
      cJSON_AddStringToObject(id, "description",
                              "Symbol name to find (function, class, variable, etc.)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("identifier"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("find_symbol",
                                      "Find a code symbol (function, class, variable) across all "
                                      "indexed projects. Returns file path, line number, and kind.",
                                      s));
   }

   /* ast_grep_search */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pat = cJSON_AddObjectToObject(p, "pattern");
      cJSON_AddStringToObject(pat, "type", "string");
      cJSON_AddStringToObject(pat, "description",
                              "AST pattern to match. Use $VAR for a single node, "
                              "$$$ for multiple nodes. Examples: "
                              "'if ($COND) { return NULL; }' or 'def $FUNC($$$):'");
      cJSON *lng = cJSON_AddObjectToObject(p, "lang");
      cJSON_AddStringToObject(lng, "type", "string");
      cJSON_AddStringToObject(lng, "description",
                              "Language to search. Supported: c, cpp, python, javascript, "
                              "typescript, go, rust, java, and others supported by ast-grep.");
      cJSON *ph = cJSON_AddObjectToObject(p, "path");
      cJSON_AddStringToObject(ph, "type", "string");
      cJSON_AddStringToObject(ph, "description",
                              "File or directory to search (default: current directory).");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
      cJSON_AddItemToArray(req, cJSON_CreateString("lang"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("ast_grep_search",
                            "AST-aware structural code search using ast-grep. Finds code patterns "
                            "by structure rather than text, using meta-variables ($VAR, $$$). "
                            "More precise than regex for language-aware queries. "
                            "Falls back to a clear error if the ast-grep binary is unavailable.",
                            s));
   }
   /* delegate */
   {
      cJSON_AddItemToArray(
          tools,
          build_tool(
              "delegate",
              "Delegate a task to an aimee delegate agent instead of provider-native "
              "sub-agent tools (spawn_agent/Agent). Always async: returns a job_id; "
              "poll delegate_status for the result. Has SSH to homelab hosts and full tool "
              "execution. If already a sub-agent, return findings.",
              cJSON_Parse(
                  "{\"type\":\"object\",\"properties\":{"
                  "\"role\":{\"type\":\"string\",\"description\":\"Delegation role (e.g. code, "
                  "review, refactor, validate, test, diagnose, execute, draft, summarize)\"},"
                  "\"prompt\":{\"type\":\"string\",\"description\":\"Task for the sub-agent\"},"
                  "\"branch\":{\"type\":\"string\",\"description\":\"Optional git branch for an "
                  "isolated delegate worktree\"},"
                  "\"persona\":{\"type\":\"string\",\"description\":\"REQUIRED persona (engineer, "
                  "qa, security, reviewer, architect, or custom); sets the delegate's identity "
                  "and principles.\"},"
                  "\"cwd\":{\"type\":\"string\",\"description\":\"Optional cwd to anchor delegate "
                  "worktree isolation; defaults to the active MCP session worktree.\"}},"
                  "\"required\":[\"role\",\"prompt\",\"persona\"]}")));
   }
   /* delegate_status */
   {
      cJSON_AddItemToArray(
          tools, build_tool("delegate_status",
                            "Check a background delegate job launched by the delegate tool.",
                            cJSON_Parse("{\"type\":\"object\",\"properties\":{\"job_id\":{"
                                        "\"type\":\"integer\",\"description\":\"Background "
                                        "delegate job_id returned by delegate with "
                                        "background=true.\"}},\"required\":[\"job_id\"]}")));
   }

   /* ensemble_review */
   {
      cJSON_AddItemToArray(
          tools,
          build_tool(
              "ensemble_review",
              "Run the multi-agent roundtable in review mode against caller-provided diff "
              "text. Returns a queued run id; poll /v1/runs/{id}. The result's items "
              "describe items_round while artifact is artifact_round (the best round); "
              "compare those fields before assuming the findings match the artifact.",
              cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                          "\"diff\":{\"type\":\"string\",\"description\":\"Unified diff or code "
                          "under review.\",\"minLength\":20},"
                          "\"brief\":{\"description\":\"Optional directed review brief as a string "
                          "or object with focus/fixes/invariants/questions string arrays.\","
                          "\"anyOf\":[{\"type\":\"string\"},{\"type\":\"object\","
                          "\"properties\":{\"focus\":{\"type\":\"array\",\"items\":{\"type\":"
                          "\"string\"}},\"fixes\":{\"type\":\"array\",\"items\":{\"type\":"
                          "\"string\"}},\"invariants\":{\"type\":\"array\",\"items\":{\"type\":"
                          "\"string\"}},\"questions\":{\"type\":\"array\",\"items\":{\"type\":"
                          "\"string\"}}}}]},"
                          "\"rounds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16,"
                          "\"description\":\"Max review rounds.\"},"
                          "\"turns\":{\"type\":\"string\",\"enum\":[\"parallel\",\"sequential\"],"
                          "\"description\":\"Round execution mode.\"}},"
                          "\"required\":[\"diff\"]}")));
   }

#include "mcp_tools_pipeline.inc"

   /* preview_blast_radius */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pj = cJSON_AddObjectToObject(p, "project");
      cJSON_AddStringToObject(pj, "type", "string");
      cJSON_AddStringToObject(pj, "description", "Project name from aimee index");
      cJSON *pp = cJSON_AddObjectToObject(p, "paths");
      cJSON_AddStringToObject(pp, "type", "array");
      cJSON *pi = cJSON_CreateObject();
      cJSON_AddStringToObject(pi, "type", "string");
      cJSON_AddItemToObject(pp, "items", pi);
      cJSON_AddStringToObject(pp, "description", "File paths to preview blast radius for");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("project"));
      cJSON_AddItemToArray(req, cJSON_CreateString("paths"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("preview_blast_radius",
                            "Preview the blast radius of proposed file changes before starting "
                            "work. Returns affected files, severity, and warnings.",
                            s));
   }

   /* delegate_reply */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *did = cJSON_AddObjectToObject(p, "delegation_id");
      cJSON_AddStringToObject(did, "type", "string");
      cJSON_AddStringToObject(did, "description", "ID of the delegation to reply to");
      cJSON *ct = cJSON_AddObjectToObject(p, "content");
      cJSON_AddStringToObject(ct, "type", "string");
      cJSON_AddStringToObject(ct, "description", "Reply content for the delegate");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("delegation_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("delegate_reply",
                                             "Reply to a delegate that has requested input. "
                                             "The delegate will receive this as the response "
                                             "to its request_input call.",
                                             s));
   }

   /* record_attempt */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *tc = cJSON_AddObjectToObject(p, "task_context");
      cJSON_AddStringToObject(tc, "type", "string");
      cJSON_AddStringToObject(tc, "description", "Brief description of what was being attempted");
      cJSON *ap = cJSON_AddObjectToObject(p, "approach");
      cJSON_AddStringToObject(ap, "type", "string");
      cJSON_AddStringToObject(ap, "description", "What was tried");
      cJSON *oc = cJSON_AddObjectToObject(p, "outcome");
      cJSON_AddStringToObject(oc, "type", "string");
      cJSON_AddStringToObject(oc, "description",
                              "What happened (error message, test failure, etc.)");
      cJSON *ls = cJSON_AddObjectToObject(p, "lesson");
      cJSON_AddStringToObject(ls, "type", "string");
      cJSON_AddStringToObject(ls, "description", "What to avoid or do differently");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("approach"));
      cJSON_AddItemToArray(req, cJSON_CreateString("outcome"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("record_attempt",
                                             "Record a failed approach so that delegates can avoid "
                                             "repeating the same mistake. Stored per-session.",
                                             s));
   }

   /* list_attempts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *f = cJSON_AddObjectToObject(p, "filter");
      cJSON_AddStringToObject(f, "type", "string");
      cJSON_AddStringToObject(f, "description",
                              "Optional keyword to filter attempts by task_context or approach");
      cJSON_AddItemToArray(tools, build_tool("list_attempts",
                                             "List previously recorded failed approaches for the "
                                             "current session. Helps avoid repeating mistakes.",
                                             s));
   }

   /* store_workflow */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *proj = cJSON_AddObjectToObject(p, "project");
      cJSON_AddStringToObject(proj, "type", "string");
      cJSON_AddStringToObject(
          proj, "description",
          "Workspace/project name. Optional — defaults to the cwd-matched workspace.");
      cJSON *sig = cJSON_AddObjectToObject(p, "signal_type");
      cJSON_AddStringToObject(sig, "type", "string");
      cJSON_AddStringToObject(sig, "description",
                              "Short slug identifying the rule (e.g. 'pr-target', 'deploy', "
                              "'test-command', 'merge-flow', 'convention').");
      cJSON *rule = cJSON_AddObjectToObject(p, "rule");
      cJSON_AddStringToObject(rule, "type", "string");
      cJSON_AddStringToObject(rule, "description",
                              "The workflow rule sentence (e.g. 'PRs target testing branch').");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("signal_type"));
      cJSON_AddItemToArray(req, cJSON_CreateString("rule"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("store_workflow",
                            "Store a project workflow rule learned from the current interaction. "
                            "Deduplicated per (project, signal_type). Use for branch strategy, "
                            "test commands, deploy procedures, or merge conventions.",
                            s));
   }

   /* learning_propose */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *sig = cJSON_AddObjectToObject(p, "signal_type");
      cJSON_AddStringToObject(sig, "type", "string");
      cJSON_AddStringToObject(sig, "description",
                              "Explicit signal type: thumb_up, thumb_down, correction, "
                              "preference_statement, mark_rule, or workflow_repetition.");
      cJSON *pol = cJSON_AddObjectToObject(p, "polarity");
      cJSON_AddStringToObject(pol, "type", "string");
      cJSON *title = cJSON_AddObjectToObject(p, "title");
      cJSON_AddStringToObject(title, "type", "string");
      cJSON *desc = cJSON_AddObjectToObject(p, "description");
      cJSON_AddStringToObject(desc, "type", "string");
      cJSON *target_key = cJSON_AddObjectToObject(p, "target_key");
      cJSON_AddStringToObject(target_key, "type", "string");
      cJSON *target_mem = cJSON_AddObjectToObject(p, "target_memory_id");
      cJSON_AddStringToObject(target_mem, "type", "integer");
      cJSON *corr = cJSON_AddObjectToObject(p, "correction_text");
      cJSON_AddStringToObject(corr, "type", "string");
      cJSON *proj = cJSON_AddObjectToObject(p, "workflow_project");
      cJSON_AddStringToObject(proj, "type", "string");
      cJSON *wsig = cJSON_AddObjectToObject(p, "workflow_signal_type");
      cJSON_AddStringToObject(wsig, "type", "string");
      cJSON *ev = cJSON_AddObjectToObject(p, "evidence_refs");
      cJSON_AddStringToObject(ev, "type", "array");
      cJSON *ev_item = cJSON_CreateObject();
      cJSON_AddStringToObject(ev_item, "type", "integer");
      cJSON_AddItemToObject(ev, "items", ev_item);
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("signal_type"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("learning_propose",
                            "Inject an explicit learning signal into the router. The router "
                            "writes append-only signals, opens gated proposals, and only commits "
                            "durable sinks after corroboration or explicit accept.",
                            s));
   }

   /* learning_review */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *state = cJSON_AddObjectToObject(p, "state");
      cJSON_AddStringToObject(state, "type", "string");
      cJSON_AddStringToObject(state, "description",
                              "Proposal state filter: pending (default), committed, archived.");
      cJSON *sink = cJSON_AddObjectToObject(p, "sink");
      cJSON_AddStringToObject(sink, "type", "string");
      cJSON_AddStringToObject(sink, "description", "Optional sink filter.");
      cJSON *limit = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(limit, "type", "integer");
      cJSON_AddStringToObject(limit, "description", "Maximum proposals to return (default 20).");
      cJSON_AddItemToArray(
          tools,
          build_tool("learning_review",
                     "List learning proposals with evidence refs and current gate state.", s));
   }

   /* --- Note tools --- */

   /* create_note */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *t = cJSON_AddObjectToObject(p, "title");
      cJSON_AddStringToObject(t, "type", "string");
      cJSON_AddStringToObject(t, "description",
                              "Note title. If a note with this title exists, content is appended.");
      cJSON *c = cJSON_AddObjectToObject(p, "content");
      cJSON_AddStringToObject(c, "type", "string");
      cJSON_AddStringToObject(c, "description", "Markdown content for the note");
      cJSON *tg = cJSON_AddObjectToObject(p, "tags");
      cJSON_AddStringToObject(tg, "type", "string");
      cJSON_AddStringToObject(tg, "description", "Comma-separated tags (e.g. 'debugging,auth')");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("title"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("create_note",
                            "Create or append to an investigation note. Notes capture findings, "
                            "hypotheses, and reasoning during debugging. If a note with the same "
                            "title already exists, the new content is appended.",
                            s));
   }

   /* list_notes */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *tg = cJSON_AddObjectToObject(p, "tag");
      cJSON_AddStringToObject(tg, "type", "string");
      cJSON_AddStringToObject(tg, "description", "Filter notes by tag");
      cJSON *lm = cJSON_AddObjectToObject(p, "limit");
      cJSON_AddStringToObject(lm, "type", "integer");
      cJSON_AddStringToObject(lm, "description", "Maximum notes to return (default 20)");
      cJSON_AddItemToArray(tools,
                           build_tool("list_notes",
                                      "List investigation notes, optionally filtered by tag. "
                                      "Returns titles, tags, and creation dates.",
                                      s));
   }

   /* search_notes */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Search term to find in note titles and content");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("search_notes",
                                             "Search investigation notes by content or title. "
                                             "Returns matching notes with their content.",
                                             s));
   }

   /* --- Git tools --- */

   /* git_status */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools,
          build_tool("git_status",
                     "Get compact working tree status: branch, staged/modified/untracked counts "
                     "and file lists. Use instead of running 'git status' via Bash.",
                     s));
   }

   /* git_commit */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description", "Commit message");
      cJSON *f = cJSON_AddObjectToObject(p, "files");
      cJSON_AddStringToObject(f, "type", "array");
      cJSON *fi = cJSON_CreateObject();
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON_AddItemToObject(f, "items", fi);
      cJSON_AddStringToObject(
          f, "description",
          "Files to stage. If omitted, stages all modified tracked files. "
          "Sensitive files (.env, credentials, keys) are automatically skipped.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("message"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("git_commit",
                            "Stage files and commit. Returns commit hash and one-line diffstat. "
                            "Use instead of running 'git add' + 'git commit' via Bash.",
                            s));
   }

   /* git_push */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *f = cJSON_AddObjectToObject(p, "force");
      cJSON_AddStringToObject(f, "type", "boolean");
      cJSON_AddStringToObject(f, "description",
                              "Use --force-with-lease (default false). Never uses --force.");
      cJSON *m = cJSON_AddObjectToObject(p, "mirror");
      cJSON_AddStringToObject(m, "type", "boolean");
      cJSON_AddStringToObject(m, "description",
                              "Push with --mirror: replaces ALL remote refs with local refs. "
                              "Deletes remote branches that don't exist locally. "
                              "DESTRUCTIVE — only use when explicitly requested.");
      cJSON_AddItemToArray(tools,
                           build_tool("git_push",
                                      "Push current branch to origin. Sets upstream on first "
                                      "push. Use mirror=true to sync all refs (destructive). "
                                      "Use instead of 'git push' via Bash.",
                                      s));
   }

   /* git_verify */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");

      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(
          a, "description",
          "Action to perform: run (default), check, conflicts, env, prepare-pr, status");
      cJSON *o = cJSON_AddArrayToObject(a, "enum");
      cJSON_AddItemToArray(o, cJSON_CreateString("run"));
      cJSON_AddItemToArray(o, cJSON_CreateString("check"));
      cJSON_AddItemToArray(o, cJSON_CreateString("conflicts"));
      cJSON_AddItemToArray(o, cJSON_CreateString("env"));
      cJSON_AddItemToArray(o, cJSON_CreateString("prepare-pr"));
      cJSON_AddItemToArray(o, cJSON_CreateString("status"));

      cJSON *as = cJSON_AddObjectToObject(p, "async");
      cJSON_AddStringToObject(as, "type", "boolean");
      cJSON_AddStringToObject(as, "description",
                              "For action=run: defaults to true (async) — verification runs in the "
                              "background and returns a job_id immediately. Pass false to force "
                              "synchronous execution. For other actions, defaults to false.");

      cJSON *ji = cJSON_AddObjectToObject(p, "job_id");
      cJSON_AddStringToObject(ji, "type", "integer");
      cJSON_AddStringToObject(ji, "description",
                              "Job ID to check status for (used with action=status)");

      cJSON *ba = cJSON_AddObjectToObject(p, "base");
      cJSON_AddStringToObject(ba, "type", "string");
      cJSON_AddStringToObject(ba, "description", "Base branch for prepare-pr (default: main)");

      cJSON *pa = cJSON_AddObjectToObject(p, "path");
      cJSON_AddStringToObject(pa, "type", "string");
      cJSON_AddStringToObject(
          pa, "description",
          "Absolute path to the repo to verify (e.g. /home/user/dev/SmoothNAS). "
          "Consumed by the git dispatch layer, which also applies the session's worktree "
          "mapping — so passing the main repo root redirects to the session's worktree "
          "automatically. Omit to use the session's current tracked directory.");

      cJSON_AddItemToArray(
          tools, build_tool("git_verify",
                            "Project verification and health tool. Supports parallel runs, "
                            "async jobs, conflict resolution, and environment checks.",
                            s));
   }

   /* git_branch */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description",
                              "One of: create, switch, list, delete, claim, orphan. "
                              "Use 'claim' to take ownership of an unowned branch. "
                              "Use 'orphan' to create a branch with no history.");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Branch name (required for create/switch/delete)");
      cJSON *b = cJSON_AddObjectToObject(p, "base");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Base ref for create (default: current HEAD)");
      cJSON *bf = cJSON_AddObjectToObject(p, "force");
      cJSON_AddStringToObject(bf, "type", "boolean");
      cJSON_AddStringToObject(bf, "description",
                              "Force delete with -D instead of -d (default false). "
                              "Only for delete action.");
      cJSON *br = cJSON_AddObjectToObject(p, "remote");
      cJSON_AddStringToObject(br, "type", "boolean");
      cJSON_AddStringToObject(br, "description",
                              "Also delete the remote branch (default false). "
                              "Only for delete action.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("action"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("git_branch",
                                      "Create, switch, list, delete, claim, or orphan branches. "
                                      "Branches are owned by the creating session. "
                                      "Use 'claim' to take ownership of an unowned branch. "
                                      "Use 'orphan' to create a branch with no parent history.",
                                      s));
   }

   /* git_log */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *c = cJSON_AddObjectToObject(p, "count");
      cJSON_AddStringToObject(c, "type", "integer");
      cJSON_AddStringToObject(c, "description", "Number of commits (default 10, max 50)");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description",
                              "Ref or range (e.g. 'main..HEAD'). Default: current branch");
      cJSON *ds = cJSON_AddObjectToObject(p, "diff_stat");
      cJSON_AddStringToObject(ds, "type", "boolean");
      cJSON_AddStringToObject(ds, "description", "Include diffstat per commit (default false)");
      cJSON_AddItemToArray(tools,
                           build_tool("git_log",
                                      "Compact commit log with short hash and relative date. "
                                      "Use instead of 'git log' via Bash.",
                                      s));
   }

   /* git_diff_summary */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description",
                              "Compare against this ref (default: unstaged changes vs HEAD)");
      cJSON *so = cJSON_AddObjectToObject(p, "stat_only");
      cJSON_AddStringToObject(so, "type", "boolean");
      cJSON_AddStringToObject(so, "description",
                              "Only show file-level stats (default true). "
                              "Set false for per-file change summaries.");
      cJSON *f = cJSON_AddObjectToObject(p, "files");
      cJSON_AddStringToObject(f, "type", "array");
      cJSON *fi = cJSON_CreateObject();
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON_AddItemToObject(f, "items", fi);
      cJSON_AddStringToObject(f, "description", "Limit diff to these files");
      cJSON_AddItemToArray(
          tools, build_tool("git_diff_summary",
                            "Compact diff summary: file stats or compressed change descriptions. "
                            "Use instead of 'git diff' via Bash.",
                            s));
   }

   /* git_pr */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(
          a, "description",
          "One of: create, view, list, edit, checks, watch, merge_status, wait. "
          "'wait' polls until all checks settle and returns a pass/fail summary.");
      cJSON *t = cJSON_AddObjectToObject(p, "title");
      cJSON_AddStringToObject(t, "type", "string");
      cJSON_AddStringToObject(t, "description", "PR title (for create/edit)");
      cJSON *bd = cJSON_AddObjectToObject(p, "body");
      cJSON_AddStringToObject(bd, "type", "string");
      cJSON_AddStringToObject(bd, "description", "PR body (for create/edit)");
      cJSON *n = cJSON_AddObjectToObject(p, "number");
      cJSON_AddStringToObject(n, "type", "integer");
      cJSON_AddStringToObject(n, "description",
                              "PR number (for view/edit/checks/watch/merge_status/wait)");
      cJSON *b = cJSON_AddObjectToObject(p, "base");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Base branch for create/edit (default: main)");
      cJSON *wt = cJSON_AddObjectToObject(p, "wait");
      cJSON_AddStringToObject(wt, "type", "boolean");
      cJSON_AddStringToObject(wt, "description",
                              "For checks action: poll until all checks settle and return a "
                              "pass/fail summary instead of streaming output (default: false)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("action"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("git_pr",
                            "Create, view, list, edit, and monitor PRs. "
                            "Use instead of 'gh pr' via Bash. Essential for checking if a PR "
                            "is merged before pushing.",
                            s));
   }

   /* git_issue */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description", "One of: list (default: list)");
      cJSON *st = cJSON_AddObjectToObject(p, "state");
      cJSON_AddStringToObject(st, "type", "string");
      cJSON_AddStringToObject(st, "description",
                              "Filter by state: open, closed, all (default: open)");
      cJSON_AddItemToArray(tools, build_tool("git_issue",
                                             "List GitHub issues. "
                                             "Use instead of 'gh issue list' via Bash.",
                                             s));
   }

   /* git_pull */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "rebase");
      cJSON_AddStringToObject(r, "type", "boolean");
      cJSON_AddStringToObject(r, "description", "Use --rebase instead of merge (default false)");
      cJSON_AddItemToArray(tools,
                           build_tool("git_pull",
                                      "Pull changes from remote. Returns summary of what changed. "
                                      "Use instead of 'git pull' via Bash.",
                                      s));
   }

   /* git_clone */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *u = cJSON_AddObjectToObject(p, "url");
      cJSON_AddStringToObject(u, "type", "string");
      cJSON_AddStringToObject(u, "description", "Repository URL to clone");
      cJSON *pa = cJSON_AddObjectToObject(p, "path");
      cJSON_AddStringToObject(pa, "type", "string");
      cJSON_AddStringToObject(pa, "description", "Local path to clone into (optional)");
      cJSON *b = cJSON_AddObjectToObject(p, "branch");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Branch to checkout (optional)");
      cJSON *d = cJSON_AddObjectToObject(p, "depth");
      cJSON_AddStringToObject(d, "type", "integer");
      cJSON_AddStringToObject(d, "description", "Shallow clone depth (optional)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("url"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("git_clone", "Clone a repository. Use instead of 'git clone' via Bash.", s));
   }

   /* git_stash */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description",
                              "One of: push, pop, apply, list, drop (default: push)");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description", "Stash message (for push)");
      cJSON *idx = cJSON_AddObjectToObject(p, "index");
      cJSON_AddStringToObject(idx, "type", "integer");
      cJSON_AddStringToObject(idx, "description", "Stash index for apply/drop (default: 0)");
      cJSON_AddItemToArray(tools, build_tool("git_stash",
                                             "Stash or restore uncommitted changes. "
                                             "Use instead of 'git stash' via Bash.",
                                             s));
   }

   /* git_tag */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description", "One of: create, list, delete (default: list)");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Tag name (required for create/delete)");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description",
                              "Tag message for annotated tag (optional, for create)");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Ref to tag (default: HEAD)");
      cJSON_AddItemToArray(
          tools, build_tool("git_tag",
                            "Create, list, or delete tags. Use instead of 'git tag' via Bash.", s));
   }

   /* git_fetch */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pr = cJSON_AddObjectToObject(p, "prune");
      cJSON_AddStringToObject(pr, "type", "boolean");
      cJSON_AddStringToObject(pr, "description",
                              "Prune remote-tracking refs that no longer exist (default false)");
      cJSON *r = cJSON_AddObjectToObject(p, "remote");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Remote name (default: origin)");
      cJSON_AddItemToArray(
          tools,
          build_tool("git_fetch",
                     "Fetch from remote without merging. Use instead of 'git fetch' via Bash.", s));
   }

   /* git_reset */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Target ref (default: HEAD~1)");
      cJSON *m = cJSON_AddObjectToObject(p, "mode");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description",
                              "Reset mode: soft (keep staged), mixed (unstage, default), "
                              "or hard (discard all changes)");
      cJSON_AddItemToArray(tools,
                           build_tool("git_reset",
                                      "Reset HEAD to a ref. Use instead of 'git reset' via Bash. "
                                      "Be cautious with --hard as it discards changes.",
                                      s));
   }

   /* git_restore */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *f = cJSON_AddObjectToObject(p, "files");
      cJSON_AddStringToObject(f, "type", "array");
      cJSON *fi = cJSON_CreateObject();
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON_AddItemToObject(f, "items", fi);
      cJSON_AddStringToObject(f, "description", "Files to restore");
      cJSON *st = cJSON_AddObjectToObject(p, "staged");
      cJSON_AddStringToObject(st, "type", "boolean");
      cJSON_AddStringToObject(st, "description",
                              "Unstage files (restore --staged). Default false.");
      cJSON *src = cJSON_AddObjectToObject(p, "source");
      cJSON_AddStringToObject(src, "type", "string");
      cJSON_AddStringToObject(src, "description",
                              "Restore from this ref instead of index (e.g. HEAD~1)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("files"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("git_restore",
                            "Restore files to a previous state or unstage them. "
                            "Use instead of 'git restore' / 'git checkout -- file' via Bash.",
                            s));
   }

   /* job_start */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pi = cJSON_AddObjectToObject(p, "plan_id");
      cJSON_AddStringToObject(pi, "type", "integer");
      cJSON_AddStringToObject(pi, "description", "Plan ID to create a coordinated job from");
      cJSON *mc = cJSON_AddObjectToObject(p, "max_concurrent");
      cJSON_AddStringToObject(mc, "type", "integer");
      cJSON_AddStringToObject(mc, "description", "Maximum concurrent delegates (default 3)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("plan_id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("job_start",
                            "Queue a coordinated job from an execution plan. The queue records "
                            "per-task file ownership and conflict prevention; use job_status to "
                            "inspect claims and progress.",
                            s));
   }

   /* job_status */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *ji = cJSON_AddObjectToObject(p, "job_id");
      cJSON_AddStringToObject(ji, "type", "integer");
      cJSON_AddStringToObject(ji, "description", "Coordinated job ID to check status of");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("job_id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("job_status",
                            "Get status of a coordinated parallel job, including per-task progress "
                            "and file conflict information.",
                            s));
   }

   /* autopilot */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *act = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(act, "type", "string");
      cJSON_AddStringToObject(
          act, "description",
          "Action: start, advance, status, list, cancel, resume, link-plan, link-job");
      cJSON *tsk = cJSON_AddObjectToObject(p, "task");
      cJSON_AddStringToObject(tsk, "type", "string");
      cJSON_AddStringToObject(tsk, "description", "Task description (required for start)");
      cJSON *pd = cJSON_AddObjectToObject(p, "plan_depth");
      cJSON_AddStringToObject(pd, "type", "string");
      cJSON_AddStringToObject(
          pd, "description",
          "Optional planning depth override for start: trivial, simple, or complex");
      cJSON *pid = cJSON_AddObjectToObject(p, "pipeline_id");
      cJSON_AddStringToObject(pid, "type", "integer");
      cJSON_AddStringToObject(pid, "description", "Pipeline ID (required for most actions)");
      cJSON *plid = cJSON_AddObjectToObject(p, "plan_id");
      cJSON_AddStringToObject(plid, "type", "integer");
      cJSON_AddStringToObject(plid, "description", "Execution plan ID (for link-plan)");
      cJSON *jid = cJSON_AddObjectToObject(p, "job_id");
      cJSON_AddStringToObject(jid, "type", "integer");
      cJSON_AddStringToObject(jid, "description", "Coord job ID (for link-job)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("action"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("autopilot",
                     "Autonomous end-to-end pipeline. Manages classify→clarify→plan→execute→qa→"
                     "validate phases with circuit breakers. Use 'start' to create a pipeline, "
                     "'advance' to move it forward, 'status' to check progress.",
                     s));
   }

   /* run_background_process */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pcmd = cJSON_AddObjectToObject(p, "command");
      cJSON_AddStringToObject(pcmd, "type", "string");
      cJSON_AddStringToObject(pcmd, "description", "Shell command to run in the background");
      cJSON *pcwd = cJSON_AddObjectToObject(p, "cwd");
      cJSON_AddStringToObject(pcwd, "type", "string");
      cJSON_AddStringToObject(pcwd, "description",
                              "Working directory (optional, defaults to current)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("command"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("run_background_process",
                                      "Start a shell command in the background. Returns a process "
                                      "ID. Use get_background_output to read output.",
                                      s));
   }

   /* get_background_output */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pid = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(pid, "type", "integer");
      cJSON_AddStringToObject(pid, "description", "Process ID from run_background_process");
      cJSON *ptail = cJSON_AddObjectToObject(p, "tail_lines");
      cJSON_AddStringToObject(ptail, "type", "integer");
      cJSON_AddStringToObject(ptail, "description", "Lines to return (default 50, max 500)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("get_background_output",
                            "Get recent stdout/stderr output from a background process.", s));
   }

   /* kill_background_process */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pid = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(pid, "type", "integer");
      cJSON_AddStringToObject(pid, "description", "Process ID to kill");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("kill_background_process",
                                             "Send SIGTERM to a running background process.", s));
   }

   /* list_background_processes */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools,
          build_tool("list_background_processes",
                     "List all background processes with their status, PID, and exit code.", s));
   }

   /* rules_propose */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pt = cJSON_AddObjectToObject(p, "text");
      cJSON_AddStringToObject(pt, "type", "string");
      cJSON_AddStringToObject(pt, "description",
                              "Rule text (max 160 chars). A clear, actionable directive.");
      cJSON *pr = cJSON_AddObjectToObject(p, "reason");
      cJSON_AddStringToObject(pr, "type", "string");
      cJSON_AddStringToObject(pr, "description", "Why this rule is needed (max 240 chars).");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("text"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("rules_propose",
                            "Propose a new collaborative rule for human approval. Rules coordinate "
                            "behavior across all agents in this workspace. Max 10 active rules.",
                            s));
   }

   /* rules_list */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("rules_list",
                            "List all collaborative rules (proposed, active, rejected, retired) "
                            "with their current epoch number.",
                            s));
   }

   /* clarify_start */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pd = cJSON_AddObjectToObject(p, "description");
      cJSON_AddStringToObject(pd, "type", "string");
      cJSON_AddStringToObject(pd, "description",
                              "Vague or ambiguous task description to clarify (max 512 chars).");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("description"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("clarify_start",
                            "Start a structured clarification session for a vague task. Returns "
                            "a session id and the first targeted question for the weakest "
                            "ambiguity dimension.",
                            s));
   }

   /* clarify_answer */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pid = cJSON_AddObjectToObject(p, "session_id");
      cJSON_AddStringToObject(pid, "type", "integer");
      cJSON_AddStringToObject(pid, "description", "Clarification session id.");
      cJSON *pa = cJSON_AddObjectToObject(p, "answer");
      cJSON_AddStringToObject(pa, "type", "string");
      cJSON_AddStringToObject(pa, "description", "Answer to the current open question.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("session_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("answer"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("clarify_answer",
                            "Record an answer for the current open question in a clarification "
                            "session. If the clarity score reaches the threshold, returns the "
                            "crystallized task spec. Otherwise returns the next question.",
                            s));
   }

   /* diagnose_start */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *sy = cJSON_AddObjectToObject(p, "symptom");
      cJSON_AddStringToObject(sy, "type", "string");
      cJSON_AddStringToObject(sy, "description", "Symptom being investigated.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("symptom"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("diagnose_start",
                            "Start a structured evidence-driven diagnosis session. Returns a "
                            "diagnosis_id used to record observations, hypotheses, and evidence.",
                            s));
   }

   /* diagnose_observe */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *did = cJSON_AddObjectToObject(p, "diagnosis_id");
      cJSON_AddStringToObject(did, "type", "integer");
      cJSON *ct = cJSON_AddObjectToObject(p, "content");
      cJSON_AddStringToObject(ct, "type", "string");
      cJSON *src = cJSON_AddObjectToObject(p, "source");
      cJSON_AddStringToObject(src, "type", "string");
      cJSON_AddStringToObject(src, "description", "Optional origin (file:line, log path, etc).");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("diagnosis_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("diagnose_observe",
                            "Record an observation (symptom fact) against a diagnosis.", s));
   }

   /* diagnose_hypothesize */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *did = cJSON_AddObjectToObject(p, "diagnosis_id");
      cJSON_AddStringToObject(did, "type", "integer");
      cJSON *ct = cJSON_AddObjectToObject(p, "content");
      cJSON_AddStringToObject(ct, "type", "string");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("diagnosis_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("diagnose_hypothesize",
                                             "Add a candidate hypothesis to a diagnosis.", s));
   }

   /* diagnose_evidence */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *did = cJSON_AddObjectToObject(p, "diagnosis_id");
      cJSON_AddStringToObject(did, "type", "integer");
      cJSON *hid = cJSON_AddObjectToObject(p, "hypothesis_id");
      cJSON_AddStringToObject(hid, "type", "integer");
      cJSON *kn = cJSON_AddObjectToObject(p, "stance");
      cJSON_AddStringToObject(kn, "type", "string");
      cJSON_AddStringToObject(kn, "description", "'for' or 'against'.");
      cJSON *ct = cJSON_AddObjectToObject(p, "content");
      cJSON_AddStringToObject(ct, "type", "string");
      cJSON *rk = cJSON_AddObjectToObject(p, "rank");
      cJSON_AddStringToObject(rk, "type", "string");
      cJSON_AddStringToObject(rk, "description",
                              "Evidence rank: direct, log, code, or speculation (default code).");
      cJSON *src = cJSON_AddObjectToObject(p, "source");
      cJSON_AddStringToObject(src, "type", "string");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("diagnosis_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("hypothesis_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("stance"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("diagnose_evidence",
                     "Attach evidence for or against a hypothesis, with an evidence-strength rank.",
                     s));
   }

   /* diagnose_status */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *did = cJSON_AddObjectToObject(p, "diagnosis_id");
      cJSON_AddStringToObject(did, "type", "integer");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("diagnosis_id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("diagnose_status",
                            "Return full state of a diagnosis with ranked hypotheses (JSON).", s));
   }

   /* search_docs */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description",
                              "What you want to know about the project — a question or topic");
      cJSON *mx = cJSON_AddObjectToObject(p, "max_results");
      cJSON_AddStringToObject(mx, "type", "integer");
      cJSON_AddStringToObject(mx, "description", "Maximum passages to return (default 3, max 8)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool(
              "search_docs",
              "Search project documentation for relevant context. Use when you need to "
              "understand project architecture, APIs, design decisions, or domain concepts. "
              "Returns matching passages with source attribution. Requires the documentation "
              "index to be available; if it is unavailable, server-side maintenance is "
              "required.",
              s));
   }

   /* lsp_diagnostics */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *ws = cJSON_AddObjectToObject(p, "workspace");
      cJSON_AddStringToObject(ws, "type", "string");
      cJSON_AddStringToObject(ws, "description",
                              "Absolute path to the workspace root (defaults to the current "
                              "aimee workspace)");
      cJSON *f = cJSON_AddObjectToObject(p, "file");
      cJSON_AddStringToObject(f, "type", "string");
      cJSON_AddStringToObject(f, "description",
                              "Absolute path to a specific file to filter diagnostics for "
                              "(omit to get all workspace diagnostics)");
      cJSON_AddItemToArray(
          tools,
          build_tool("lsp_diagnostics",
                     "Return current LSP diagnostics (errors and warnings) for a file or the "
                     "entire workspace. Requires an LSP server to be configured in aimee.yaml "
                     "lsp_servers. Returns structured diagnostics with file, line, column, "
                     "severity, and message.",
                     s));
   }

   /* lsp_definition */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *ws = cJSON_AddObjectToObject(p, "workspace");
      cJSON_AddStringToObject(ws, "type", "string");
      cJSON_AddStringToObject(ws, "description", "Absolute path to the workspace root");
      cJSON *f = cJSON_AddObjectToObject(p, "file");
      cJSON_AddStringToObject(f, "type", "string");
      cJSON_AddStringToObject(f, "description", "Absolute path to the file containing the symbol");
      cJSON *ln = cJSON_AddObjectToObject(p, "line");
      cJSON_AddStringToObject(ln, "type", "integer");
      cJSON_AddStringToObject(ln, "description", "Line number (0-based) of the symbol");
      cJSON *col = cJSON_AddObjectToObject(p, "col");
      cJSON_AddStringToObject(col, "type", "integer");
      cJSON_AddStringToObject(col, "description", "Column number (0-based) of the symbol");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("file"));
      cJSON_AddItemToArray(req, cJSON_CreateString("line"));
      cJSON_AddItemToArray(req, cJSON_CreateString("col"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("lsp_definition",
                     "Go to the definition of a symbol at a given file position. Returns the "
                     "target file and line number. Faster and more precise than grep heuristics.",
                     s));
   }

   /* lsp_references */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *ws = cJSON_AddObjectToObject(p, "workspace");
      cJSON_AddStringToObject(ws, "type", "string");
      cJSON_AddStringToObject(ws, "description", "Absolute path to the workspace root");
      cJSON *f = cJSON_AddObjectToObject(p, "file");
      cJSON_AddStringToObject(f, "type", "string");
      cJSON_AddStringToObject(f, "description", "Absolute path to the file containing the symbol");
      cJSON *ln = cJSON_AddObjectToObject(p, "line");
      cJSON_AddStringToObject(ln, "type", "integer");
      cJSON_AddStringToObject(ln, "description", "Line number (0-based) of the symbol");
      cJSON *col = cJSON_AddObjectToObject(p, "col");
      cJSON_AddStringToObject(col, "type", "integer");
      cJSON_AddStringToObject(col, "description", "Column number (0-based) of the symbol");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("file"));
      cJSON_AddItemToArray(req, cJSON_CreateString("line"));
      cJSON_AddItemToArray(req, cJSON_CreateString("col"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("lsp_references",
                     "Find all references to the symbol at a given file position across the "
                     "workspace. Returns a list of file:line locations. Useful before renaming "
                     "or removing a symbol.",
                     s));
   }

   /* session_start */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *t = cJSON_AddObjectToObject(p, "template");
      cJSON_AddStringToObject(t, "type", "string");
      cJSON_AddStringToObject(
          t, "description",
          "Workflow template name (e.g. code-review, debate, planning, "
          "design-critique) or a project-local template in session_templates/.");
      cJSON *c = cJSON_AddObjectToObject(p, "channel");
      cJSON_AddStringToObject(c, "type", "string");
      cJSON_AddStringToObject(c, "description",
                              "Channel name for this workflow session (default: 'general').");
      cJSON *a = cJSON_AddObjectToObject(p, "assignments");
      cJSON_AddStringToObject(a, "type", "object");
      cJSON_AddStringToObject(a, "description",
                              "Map of role name to array of agent names (e.g. "
                              "{\"reviewer\": [\"claude-1\", \"gemini\"]}). Every role "
                              "appearing in the template must have at least one agent.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("template"));
      cJSON_AddItemToArray(req, cJSON_CreateString("assignments"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("session_start",
                     "Start a templated multi-agent workflow session (code-review, debate, "
                     "planning, ...). Returns the structured session state including the first "
                     "expected agent and prompt.",
                     s));
   }

   /* session_status */
   cJSON_AddItemToArray(
       tools, build_tool("session_status",
                         "Fetch the current state of a workflow session: phase, turn, "
                         "expected participant, pause reason, and recent context excerpt.",
                         cJSON_Parse("{\"type\":\"object\","
                                     "\"properties\":{\"id\":{\"type\":\"integer\","
                                     "\"description\":\"Workflow session id\"}},"
                                     "\"required\":[\"id\"]}")));

   /* session_pause */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *i = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(i, "type", "integer");
      cJSON_AddStringToObject(i, "description", "Workflow session id");
      cJSON *r = cJSON_AddObjectToObject(p, "reason");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Reason for pausing (default: 'manual').");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("id"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("session_pause", "Pause a workflow session with an optional reason.", s));
   }

   /* session_advance */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *i = cJSON_AddObjectToObject(p, "id");
      cJSON_AddStringToObject(i, "type", "integer");
      cJSON_AddStringToObject(i, "description", "Workflow session id");
      cJSON *sp = cJSON_AddObjectToObject(p, "speaker");
      cJSON_AddStringToObject(sp, "type", "string");
      cJSON_AddStringToObject(sp, "description",
                              "Name of the agent or human speaking this turn. An assigned agent "
                              "that does not match the expected agent is rejected; any other "
                              "speaker is treated as a human interruption and pauses the session.");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description", "The message text to record for this turn.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("speaker"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("session_advance",
                                      "Record a turn in a workflow session and advance to the next "
                                      "expected participant. Returns the updated session state.",
                                      s));
   }

   /* session_list */
   cJSON_AddItemToArray(
       tools, build_tool("session_list",
                         "List every stored workflow session with its template, channel, status, "
                         "phase, and expected participant.",
                         cJSON_Parse("{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":"
                                     "\"integer\",\"description\":\"Maximum sessions to return "
                                     "(default 20, max 100).\"}}}")));

   /* mutate */
   cJSON_AddItemToArray(
       tools,
       build_tool(
           "mutate",
           "Perform a typed memory mutation. Verbs: store (write new fact), update "
           "(replace content in place), supersede (replace with version lineage), "
           "forget (retire/delete), affirm (positive reinforcement), reject (reduce confidence).",
           cJSON_Parse(
               "{\"type\":\"object\",\"properties\":{"
               "\"verb\":{\"type\":\"string\","
               "\"description\":\"Mutation verb: store|update|supersede|forget|affirm|reject\"},"
               "\"id\":{\"type\":\"integer\","
               "\"description\":\"Memory id (required for "
               "update/supersede/forget/affirm/reject)\"},"
               "\"key\":{\"type\":\"string\","
               "\"description\":\"Memory key (required for store)\"},"
               "\"content\":{\"type\":\"string\","
               "\"description\":\"Memory content (required for store/update/supersede)\"},"
               "\"tier\":{\"type\":\"string\","
               "\"description\":\"Tier: L1|L2|L3 (store, default L2)\"},"
               "\"kind\":{\"type\":\"string\","
               "\"description\":\"Kind: fact|rule|decision|preference|... (store, default fact)\"},"
               "\"confidence\":{\"type\":\"number\","
               "\"description\":\"Confidence 0.0-1.0 (store/supersede, default 1.0)\"},"
               "\"reason\":{\"type\":\"string\","
               "\"description\":\"Reason string (reject)\"}},"
               "\"required\":[\"verb\"]}")));

   add_session_context_tools(tools);

   /* Plugin tools: load registry + project-local, add enabled tools */
   {
      plugin_t plugins[PLUGIN_MAX_PLUGINS];
      int pcount = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);
      /* Also discover local plugins from configured workspaces */
      config_t pcfg;
      config_load(&pcfg);
      const char *roots[64];
      for (int ri = 0; ri < pcfg.workspace_count && ri < 64; ri++)
         roots[ri] = pcfg.workspaces[ri];
      plugin_discover_local(roots, pcfg.workspace_count, plugins, &pcount, PLUGIN_MAX_PLUGINS);

      static plugin_tool_t ptool_buf[PLUGIN_MAX_PLUGINS * PLUGIN_MAX_TOOLS];
      int ptool_count = plugin_collect_tools(plugins, pcount, ptool_buf,
                                             (int)(sizeof(ptool_buf) / sizeof(ptool_buf[0])));
      for (int pi = 0; pi < ptool_count; pi++)
      {
         const plugin_tool_t *pt = &ptool_buf[pi];
         /* Namespace: "plugin:<name>" */
         char namespaced[128];
         snprintf(namespaced, sizeof(namespaced), "plugin:%s", pt->name);

         if (tools_array_has_name(tools, pt->name) || tools_array_has_name(tools, namespaced))
         {
            LOG_WARN("mcp-tools", "plugin tool '%s' conflicts, skipped", pt->name);
            continue;
         }

         cJSON *schema = cJSON_Parse(pt->input_schema_json);
         if (!schema)
            schema = cJSON_CreateObject();

         /* Include permission level in the description */
         char desc[640];
         snprintf(desc, sizeof(desc), "%s [plugin, permission=%s]",
                  pt->description[0] ? pt->description : pt->name,
                  plugin_permission_name(pt->permission));

         cJSON_AddItemToArray(tools, build_tool(namespaced, desc, schema));
      }
   }

   cJSON *remote_tools = mcp_client_registry_build_namespaced_tools(1000);
   if (cJSON_IsArray(remote_tools))
   {
      cJSON *tool = NULL;
      cJSON_ArrayForEach(tool, remote_tools)
      {
         cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
         if (!cJSON_IsString(name) || !name->valuestring[0])
            continue;

         const char *raw_name = strchr(name->valuestring, ':');
         if (raw_name && raw_name[1] && tools_array_has_name(tools, raw_name + 1))
            LOG_WARN("mcp-tools", "remote tool name collides with builtin, namespaced as %s",
                     name->valuestring);

         cJSON_AddItemToArray(tools, cJSON_Duplicate(tool, 1));
      }
   }
   cJSON_Delete(remote_tools);

   return tools;
}
