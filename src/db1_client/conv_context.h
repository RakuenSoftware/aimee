/* db1/conv_context.h: DB1 storage layer for virtual context tool-chain events. */
#ifndef DB1_CONV_CONTEXT_H
#define DB1_CONV_CONTEXT_H 1

#include <stdint.h>

typedef struct
{
   int64_t id;
   char session_id[128];
   char tool_name[128];
   char tool_input[2048];
   char tool_result[4096];
   int result_bytes;
   int64_t chain_id;
   char created_at[32];
} conv_tool_event_t;

typedef struct
{
   int64_t id;
   char session_id[128];
   int64_t event_id_first;
   int64_t event_id_last;
   char tools[256];
   char stub[2048];
   int raw_bytes;
   int stub_bytes;
   char state[16];
   char created_at[32];
} conv_tool_chain_t;

/* Record a tool call event. Returns the new event id (>0) or -1 on error. */
int64_t db1_conv_record_event(const char *session_id, const char *tool_name, const char *tool_input,
                              const char *tool_result, int result_bytes);

/* Assign chain_id to a range of events. Returns 0 on success. */
int db1_conv_set_chain_id(int64_t event_id_first, int64_t event_id_last, int64_t chain_id);

/* Insert a new chain. Returns the new chain id (>0) or -1 on error. */
int64_t db1_conv_insert_chain(const char *session_id, int64_t event_id_first, int64_t event_id_last,
                              const char *tools, const char *stub, int raw_bytes, int stub_bytes);

/* Load events not yet assigned to a chain for a session.
 * Returns count; populates out[] up to max entries. */
int db1_conv_pending_events(const char *session_id, conv_tool_event_t *out, int max);

/* Load all chains for a session, ordered by id. Returns count. */
int db1_conv_list_chains(const char *session_id, conv_tool_chain_t *out, int max);

/* Load events for a specific chain. Returns count. */
int db1_conv_chain_events(int64_t chain_id, conv_tool_event_t *out, int max);

/* Full-text search over chain stubs. Returns count. */
int db1_conv_search_chains(const char *session_id, const char *query, conv_tool_chain_t *out,
                           int max);

/* Get or create context state for a session. Returns 0 on success. */
int db1_conv_state_get(const char *session_id, int64_t *last_event_id_out, int *chain_count_out,
                       int *event_count_out);

/* Update context state. Returns 0 on success. */
int db1_conv_state_update(const char *session_id, int64_t last_event_id, int chain_count,
                          int event_count);

#endif /* DB1_CONV_CONTEXT_H */
