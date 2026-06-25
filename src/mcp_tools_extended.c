/* mcp_tools_extended.c: P3 catalog extension.
 *
 * Read-only MCP tools that expose existing server/kb capabilities — project
 * roadmaps + task graph, code-index navigation, and memory match-explanation —
 * to external MCP clients. These were previously reachable only via /v1 or the
 * kb action surface. Definitions only; the matching content handlers live in
 * server_mcp_call_table.inc. Kept out of mcp_tools.c (which is at its line
 * budget), and added to the catalog via mcp_build_tools_list -> here. Because
 * the P2 default presentation profile is "core", these are not shown upfront but
 * are discoverable via find_tools/describe_tool and callable by name. */
#include "cJSON.h"
#include "headers/mcp_tools.h"

/* Append a {name, description, inputSchema:{type:object, properties:{}}} tool and
 * return it so the caller can attach properties / required entries. */
static cJSON *ext_tool(cJSON *tools, const char *name, const char *desc)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON *s = cJSON_CreateObject();
   cJSON_AddStringToObject(s, "type", "object");
   cJSON_AddObjectToObject(s, "properties");
   cJSON_AddItemToObject(t, "inputSchema", s);
   cJSON_AddItemToArray(tools, t);
   return t;
}

static void ext_prop(cJSON *tool, const char *key, const char *type, const char *desc)
{
   cJSON *s = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *props = cJSON_GetObjectItemCaseSensitive(s, "properties");
   cJSON *p = cJSON_AddObjectToObject(props, key);
   cJSON_AddStringToObject(p, "type", type);
   cJSON_AddStringToObject(p, "description", desc);
}

static void ext_require(cJSON *tool, const char *key)
{
   cJSON *s = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
   cJSON *req = cJSON_GetObjectItemCaseSensitive(s, "required");
   if (!req)
      req = cJSON_AddArrayToObject(s, "required");
   cJSON_AddItemToArray(req, cJSON_CreateString(key));
}

void mcp_add_extended_tools(cJSON *tools)
{
   if (!tools)
      return;
   cJSON *t;

   /* ── Planning: roadmaps + task graph ─────────────────────────────────────── */
   ext_tool(tools, "roadmap_list", "List the project's roadmaps (ids + summaries) as JSON.");

   t = ext_tool(tools, "roadmap_show",
                "Show one roadmap by id: its milestone / task tree, as JSON.");
   ext_prop(t, "roadmap_id", "string", "Roadmap id (see roadmap_list).");
   ext_require(t, "roadmap_id");

   t = ext_tool(tools, "task_list",
                "List tasks from the project task graph (id, parent, title, state, confidence).");
   ext_prop(t, "state", "string", "Filter by state (e.g. open, done). Omit for all states.");
   ext_prop(t, "session_id", "string", "Filter by originating session. Omit for all.");
   ext_prop(t, "limit", "integer", "Max tasks to return (default 100, max 500).");

   /* ── Code intelligence: index navigation ─────────────────────────────────── */
   t = ext_tool(tools, "index_find_callers",
                "Find call sites of a symbol across the indexed code: project, file, calling "
                "function, line.");
   ext_prop(t, "symbol", "string", "Symbol / function name to find callers of.");
   ext_prop(t, "project", "string", "Restrict to a project (optional; omit to search all).");
   ext_require(t, "symbol");

   t = ext_tool(tools, "index_structure",
                "List the definitions (functions / types) in an indexed file with line ranges.");
   ext_prop(t, "file_path", "string", "File path within the indexed project.");
   ext_prop(t, "project", "string", "Project the file belongs to (optional).");
   ext_require(t, "file_path");

   /* ── Memory grounding: explain a retrieval ───────────────────────────────── */
   t = ext_tool(tools, "memory_explain_match",
                "Explain WHY a memory matches a query: the per-signal score breakdown "
                "(lexical / semantic / entity / graph / cross-encoder / …).");
   ext_prop(t, "query", "string", "The query to score the memory against.");
   ext_prop(t, "memory_id", "integer", "Id of the memory to explain (e.g. from search_memory).");
   ext_require(t, "query");
   ext_require(t, "memory_id");
}
