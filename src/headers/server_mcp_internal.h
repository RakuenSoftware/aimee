#ifndef SERVER_MCP_INTERNAL_H
#define SERVER_MCP_INTERNAL_H
#include "server.h"
#include <stdio.h>
#include <string.h>
/* Cross-TU decls split from server_mcp.c. */

/* Compatibility parser for clients that serialize a structured span array item
 * as the CLI spelling `path:start-end`. Pure and header-local so the batch shape
 * can be regression-tested without linking the entire MCP dispatch table. */
static inline int server_mcp_span_shorthand_parse(const char *text, char *path, size_t path_cap,
                                                  int *line_start, int *line_end)
{
   if (!text || !path || path_cap == 0 || !line_start || !line_end)
      return 0;
   const char *colon = strrchr(text, ':');
   long long start = 0, end = 0;
   char tail = '\0';
   if (!colon || colon == text || sscanf(colon + 1, "%lld-%lld%c", &start, &end, &tail) != 2 ||
       start <= 0 || end < start)
      return 0;
   size_t path_len = (size_t)(colon - text);
   if (path_len >= path_cap)
      return 0;
   memcpy(path, text, path_len);
   path[path_len] = '\0';
   *line_start = (int)start;
   *line_end = (int)end;
   return 1;
}

/* Idempotently register an MCP session in the server_sessions registry (tagged
 * client_type "mcp"), so an MCP serve session -- which is pure tool calls and
 * never drives a chat turn -- is locatable after a crash/restart. Best-effort:
 * a NULL/empty/unsafe sid or a DB failure is a silent no-op. Called from the
 * mcp.call seam (tools/list is a bodyless GET that carries no session id). */
void mcp_session_register(server_conn_t *conn, const char *sid);

/* promoted cross-TU (former .inc statics) */
cJSON *text_content(const char *text);
cJSON *tool_ast_grep_search(cJSON *args);

/* Is a REAL ast-grep binary resolvable? Used to withhold ast_grep_search from
 * tools/list when it is not, so the surface stops advertising a search that
 * cannot run. Verified once per process and cached. */
int ast_grep_available(void);

struct mcp_call
{
   server_ctx_t *ctx;
   server_conn_t *conn;
   cJSON *jargs;
   const char *sid;
   const char *tool;
   cJSON **structured;
};

typedef cJSON *(*mcp_tool_handler_fn)(struct mcp_call *c);
cJSON *json_result_content(cJSON *result);
mcp_tool_handler_fn mcp_tool_lookup(const char *tool);

/* Give aimee's own agents the MCP tools marked native in mcp_tool_table, and
 * register the handler that runs them. Call once at server startup, BEFORE the
 * first toolset_registry_init() or build_tools_array(). See the table's header
 * comment in server_mcp_call_table.c. */
void mcp_tool_register_native_surface(void);
cJSON *tool_complete_prospective_memory(cJSON *args);
cJSON *smcp_tool_create_note(cJSON *args);
cJSON *tool_create_prospective_memory(cJSON *args);
cJSON *smcp_tool_find_symbol(cJSON *args);
cJSON *smcp_tool_search_docs(cJSON *args);
cJSON *tool_get_context_block(cJSON *args);
cJSON *tool_get_entity(cJSON *args);
cJSON *tool_get_entity_edges(cJSON *args);
cJSON *tool_get_episode(cJSON *args);
cJSON *tool_get_help(cJSON *args);
cJSON *tool_get_host(cJSON *args);
cJSON *tool_get_identity(void);
cJSON *tool_job_start(cJSON *args);
cJSON *tool_job_status(cJSON *args);
cJSON *tool_list_attempts(cJSON *args);
cJSON *tool_list_curiosity_items(cJSON *args);
cJSON *tool_list_facts(cJSON *args);
cJSON *tool_list_hosts(void);
cJSON *smcp_tool_list_notes(cJSON *args);
cJSON *tool_list_prospective_memories(cJSON *args);
cJSON *tool_memory_ask(cJSON *args, cJSON **structured_out);
cJSON *tool_memory_briefing(cJSON *args);
/* Always activates a request-local context; active_context_missing reports the
 * safe shared/global-only fallback when project/workspace resolution fails. */
void mcp_memory_scope_begin(cJSON *args, int *active_context_missing);
void mcp_memory_scope_end(void);
cJSON *tool_memory_get(cJSON *args);
cJSON *tool_memory_mutate(cJSON *args);
cJSON *tool_preview_blast_radius(cJSON *args);
cJSON *tool_record_attempt(cJSON *args);
cJSON *tool_search_graph(cJSON *args);
cJSON *tool_search_memory(cJSON *args);
cJSON *smcp_tool_search_notes(cJSON *args);
cJSON *tool_store_workflow(cJSON *args);
cJSON *json_result_content(cJSON *result);
mcp_tool_handler_fn mcp_tool_lookup(const char *tool);
cJSON *tool_complete_prospective_memory(cJSON *args);
cJSON *smcp_tool_create_note(cJSON *args);
cJSON *tool_create_prospective_memory(cJSON *args);
cJSON *smcp_tool_find_symbol(cJSON *args);
cJSON *tool_get_context_block(cJSON *args);
cJSON *tool_get_entity(cJSON *args);
cJSON *tool_get_entity_edges(cJSON *args);
cJSON *tool_get_episode(cJSON *args);
cJSON *tool_get_help(cJSON *args);
cJSON *tool_get_host(cJSON *args);
cJSON *tool_get_identity(void);
cJSON *tool_job_start(cJSON *args);
cJSON *tool_job_status(cJSON *args);
cJSON *tool_list_attempts(cJSON *args);
cJSON *tool_list_curiosity_items(cJSON *args);
cJSON *tool_list_facts(cJSON *args);
cJSON *tool_list_hosts(void);
cJSON *smcp_tool_list_notes(cJSON *args);
cJSON *tool_list_prospective_memories(cJSON *args);
cJSON *tool_memory_ask(cJSON *args, cJSON **structured_out);
cJSON *tool_memory_briefing(cJSON *args);
cJSON *tool_memory_get(cJSON *args);
cJSON *tool_memory_mutate(cJSON *args);
cJSON *tool_preview_blast_radius(cJSON *args);
cJSON *tool_record_attempt(cJSON *args);
cJSON *tool_search_graph(cJSON *args);
cJSON *tool_search_memory(cJSON *args);
cJSON *smcp_tool_search_notes(cJSON *args);
cJSON *tool_store_workflow(cJSON *args);
#endif
