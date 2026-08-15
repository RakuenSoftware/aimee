/* db1/server_sessions.h: per-machine aimee-server connection sessions.
 *
 * One row per active client connection to aimee-server. Carries
 * auth principal, client type (cli/webchat/mcp), the agent's claude
 * session id (if applicable), and run outcome for effectiveness
 * accounting. User-local state: never leaves the machine.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_SERVER_SESSIONS_H
#define DEC_DB1_SERVER_SESSIONS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_SS_ID_LEN        128
#define DB1_SS_CLIENT_LEN    32
#define DB1_SS_PRINCIPAL_LEN 64
#define DB1_SS_TITLE_LEN     256
#define DB1_SS_TS_LEN        32
#define DB1_SS_CSID_LEN      128
#define DB1_SS_OUTCOME_LEN   32
#define DB1_SS_SOURCE_LEN    64
#define DB1_SS_CHAT_KEY_LEN  256

   typedef struct
   {
      char id[DB1_SS_ID_LEN];
      char client_type[DB1_SS_CLIENT_LEN];
      char principal[DB1_SS_PRINCIPAL_LEN];
      char title[DB1_SS_TITLE_LEN];
      char created_at[DB1_SS_TS_LEN];
      char last_activity_at[DB1_SS_TS_LEN];
      char claude_session_id[DB1_SS_CSID_LEN];
      char outcome[DB1_SS_OUTCOME_LEN];
      char source[DB1_SS_SOURCE_LEN];     /* platform origin e.g. "telegram" */
      char chat_key[DB1_SS_CHAT_KEY_LEN]; /* gateway session key e.g. "telegram:dm:123" */
   } db1_server_session_t;

   /* Insert a fresh row with empty title + outcome, created_at/last_activity_at
    * set to now. Returns 0 on success, -1 on error. */
   int db1_server_session_create(const char *id, const char *client_type, const char *principal);

   /* Load session by id. Returns 0 on hit, -1 on miss or error. */
   int db1_server_session_get(const char *id, db1_server_session_t *out);

   /* UPDATE outcome. Returns 0 on success. */
   int db1_server_session_set_outcome(const char *id, const char *outcome);

   /* UPDATE source + chat_key (gateway origin). Returns 0 on success. */

   /* DELETE by id. Returns 0 on success. */
   int db1_server_session_delete(const char *id);

   /* List most recent `max` sessions, newest-last_activity first. */
   int db1_server_session_list_recent(db1_server_session_t *out, int max);

   /* Rows whose title LIKE `pattern`. */
   int db1_server_session_search_by_title(const char *pattern, db1_server_session_t *out, int max);

   /* SELECT COUNT(*), optionally filtered by created_at >= since. */
   int db1_server_session_count(const char *since_or_null);

   /* List ids of sessions older than `threshold_seconds` seconds. */
   int db1_server_session_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_ID_LEN],
                                       int max);

   /* Delete all sessions older than `threshold_seconds`. Returns rows deleted. */
   int db1_server_session_delete_expired(int threshold_seconds);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_SERVER_SESSIONS_H */
