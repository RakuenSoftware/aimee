/* db1/primary_sessions.h: durable primary-agent conversation transcripts.
 *
 * Direct primary adapters retain canonical message history server-side
 * because some providers are called with store=false. This DB1 table gives
 * those sessions restart-safe continuity without exposing sqlite outside DB1.
 */
#ifndef DEC_DB1_PRIMARY_SESSIONS_H
#define DEC_DB1_PRIMARY_SESSIONS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_PS_SESSION_ID_LEN 128
#define DB1_PS_AGENT_LEN      64
#define DB1_PS_PROVIDER_LEN   64
#define DB1_PS_TS_LEN         32

   typedef struct
   {
      char session_id[DB1_PS_SESSION_ID_LEN];
      char agent_name[DB1_PS_AGENT_LEN];
      char provider[DB1_PS_PROVIDER_LEN];
      char created_at[DB1_PS_TS_LEN];
      char updated_at[DB1_PS_TS_LEN];
      char *messages_json;
   } db1_primary_session_row_t;

   /* Canonical ordered session log. Append returns an exact decimal sequence
    * into out_sequence[32]; retrying an event ID is idempotent only when every
    * immutable field matches. Page returns owned canonical envelope-array JSON. */
   int db1_session_ir_append(const char *session_id, const char *event_id, const char *turn_id,
                             const char *actor, const char *payload, char out_sequence[32]);
   char *db1_session_ir_page(const char *session_id, const char *after, int limit,
                             const char *principal);

   /* Owner-checked host history projections in the ordered session log.
    * Save appends an atomic checkpoint; load replays the latest checkpoint and
    * subsequent completed harness observations. scope keeps child prompts private.
    * Load returns 0 found, 1 missing, -1 failure; caller frees *out. */
   int db1_session_ir_history_save(const char *session_id, const char *checkpoint,
                                   const char *scope, const char *actor, const char *messages_json);
   int db1_session_ir_history_load(const char *session_id, const char *scope, const char *actor,
                                   const char *active_turn, char **out);
   /* Owner-checked primary conversation including this running turn's committed
    * observations. Missing/non-running returns 1; corruption/outage returns -1.
    * Existing completed-history loading above keeps excluding pending content. */
   int db1_session_ir_running_history_load(const char *session_id, const char *actor,
                                           const char *active_turn, char **out);

   /* Native resume metadata; get returns 0 found, 1 missing, -1 failure. */
   int db1_session_harness_get(const char *session_id, const char *harness, char *out_native_id,
                               size_t capacity, const char *principal);
   int db1_session_harness_bind(const char *session_id, const char *harness, const char *native_id,
                                const char *principal);

   /* Atomically consume/complete host cache metadata. Empty expected claims a
    * missing key. Returns 0 exchanged, 1 stale/missing, -1 storage/refusal. */
   int db1_session_harness_compare_swap(const char *session_id, const char *harness,
                                        const char *expected, const char *replacement,
                                        const char *principal);

   /* Upsert the canonical message array JSON for one primary
    * conversation session. Returns 0 on success. */
   int db1_primary_session_save(const char *session_id, const char *agent_name,
                                const char *provider, const char *messages_json);

   /* Return a malloc'd copy of the stored message array JSON, or NULL on miss
    * or error. Caller frees. */
   char *db1_primary_session_load(const char *session_id, const char *agent_name,
                                  const char *provider);

   /* Delete one stored primary conversation session. Returns 0 on success. */
   int db1_primary_session_delete(const char *session_id, const char *agent_name,
                                  const char *provider);

   /* Allocate rows for recent or transcript-matching primary sessions. Caller
    * frees with db1_primary_session_rows_free. Returns row count or -1. */
   int db1_primary_session_alloc_recent(db1_primary_session_row_t **out, int max);
   int db1_primary_session_alloc_search(const char *query, db1_primary_session_row_t **out,
                                        int max);

   /* Load the newest transcript row for one session_id. Returns 0 on hit, 1 on miss, -1 on failure.
    */
   int db1_primary_session_get_latest(const char *session_id, db1_primary_session_row_t *out);

   void db1_primary_session_row_clear(db1_primary_session_row_t *row);
   void db1_primary_session_rows_free(db1_primary_session_row_t *rows, int count);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_PRIMARY_SESSIONS_H */
