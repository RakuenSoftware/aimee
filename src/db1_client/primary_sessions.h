/* db1/primary_sessions.h: durable primary-agent conversation transcripts.
 *
 * Direct primary adapters keep provider-formatted message history client-side
 * because some providers are called with store=false. This DB1 table gives
 * those sessions restart-safe continuity without exposing sqlite outside DB1.
 */
#ifndef DEC_DB1_PRIMARY_SESSIONS_H
#define DEC_DB1_PRIMARY_SESSIONS_H 1

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

   /* Upsert the provider-formatted message array JSON for one primary
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

   /* Load the newest transcript row for one session_id. Returns 0 on hit. */
   int db1_primary_session_get_latest(const char *session_id, db1_primary_session_row_t *out);

   void db1_primary_session_row_clear(db1_primary_session_row_t *row);
   void db1_primary_session_rows_free(db1_primary_session_row_t *rows, int count);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_PRIMARY_SESSIONS_H */
