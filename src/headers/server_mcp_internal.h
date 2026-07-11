#ifndef SERVER_MCP_INTERNAL_H
#define SERVER_MCP_INTERNAL_H
#include "server.h"
/* Cross-TU decls split from server_mcp.c. */

/* Idempotently register an MCP session in the server_sessions registry (tagged
 * client_type "mcp"), so an MCP serve session -- which is pure tool calls and
 * never drives a chat turn -- is locatable after a crash/restart. Best-effort:
 * a NULL/empty/unsafe sid or a DB failure is a silent no-op. Called from the
 * mcp.call seam (tools/list is a bodyless GET that carries no session id). */
void mcp_session_register(server_conn_t *conn, const char *sid);

/* promoted cross-TU (former .inc statics) */
cJSON *text_content(const char *text);
cJSON *tool_ast_grep_search(cJSON *args);

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
cJSON *tool_list_facts(void);
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
cJSON *tool_list_facts(void);
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
