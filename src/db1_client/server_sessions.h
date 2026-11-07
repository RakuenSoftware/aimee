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
#include "vault_principal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_SS_ID_LEN        128
#define DB1_SS_CLIENT_LEN    32
#define DB1_SS_PRINCIPAL_LEN VAULT_PRINCIPAL_MAX
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

   /* Authority/incarnation projection. Opaque display metadata cannot make an
    * otherwise valid owner unreadable; identity bounds remain exact. */
   typedef struct
   {
      char id[DB1_SS_ID_LEN], principal[DB1_SS_PRINCIPAL_LEN], created_at[DB1_SS_TS_LEN];
   } db1_server_session_owner_t;
   int db1_server_session_owner_get(const char *id, db1_server_session_owner_t *out);

   /* Complete owned metadata from the existing ten-cell session operation.
    * Every field is non-NULL after a successful read, including empty values.
    * Release each returned row with record_free; failures leave it empty. */
   typedef struct
   {
      char *id, *client_type, *principal, *title, *created_at, *last_activity_at;
      char *claude_session_id, *outcome, *source, *chat_key;
   } db1_server_session_record_t;
   int db1_server_session_record_get(const char *id, db1_server_session_record_t *out);
   int db1_server_session_record_list_recent(db1_server_session_record_t *out, int max);
   int db1_server_session_record_search_by_title(const char *pattern,
                                                 db1_server_session_record_t *out, int max);
   void db1_server_session_record_free(db1_server_session_record_t *record);

   /* Insert a fresh row with empty title + outcome, created_at/last_activity_at
    * set to now. Returns 0 on success, -1 on error. */
   int db1_server_session_create(const char *id, const char *client_type, const char *principal);

   /* Load session by id. Returns 0 on hit, -1 on miss or error. */
   int db1_server_session_get(const char *id, db1_server_session_t *out);

   /* Owner-bound durable primary agent. Empty selection means default.
    * Returns 0 only on acknowledged owner-checked read/write; -1 on error. */
   int db1_server_session_primary_get(const char *principal, const char *session_id, char *out,
                                      size_t cap);
   int db1_server_session_primary_set(const char *principal, const char *session_id,
                                      const char *agent);

   /* UPDATE source + chat_key (gateway origin). Returns 0 on success. */

   /* Atomically reserve first-message persona delivery on this durable session.
    * Returns 1 when THIS caller won the claim and must deliver, 0 when another
    * caller already claimed or completed it, -1 on error. Pair every 1 with
    * db1_server_session_persona_delivery_finish so a failed delivery does not
    * leave the session wedged mid-claim. */
   int db1_server_session_persona_delivery_claim(const char *id);

   /* Release a claim: delivered != 0 marks it done, 0 returns it to unclaimed
    * so a later request retries. Returns 0 on success. */
   int db1_server_session_persona_delivery_finish(const char *id, int delivered);

   /* DELETE by id. Returns 0 on success. */
   int db1_server_session_delete(const char *id);

   /* SELECT COUNT(*), optionally filtered by created_at >= since. */
   int db1_server_session_count(const char *since_or_null);

   /* List ids of sessions older than `threshold_seconds` seconds. */
   int db1_server_session_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_ID_LEN],
                                       int max);

   /* Delete all sessions older than `threshold_seconds`. Returns rows deleted. */
   int db1_server_session_delete_expired(int threshold_seconds);

   /* List/erase the complete mutable DB1 content graph owned by one verified
    * principal. Immutable audit/WORM rows are never members of that graph. */
   int db1_server_session_list_by_subject(const char *principal, char (*out_ids)[DB1_SS_ID_LEN],
                                          int max);
   int db1_server_session_erase_subject(const char *request_id, const char *principal);
   /* Caller owns the bounded JSON array of erased session digest locators. */
   char *db1_server_session_erasure_receipt(const char *request_id, const char *principal);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_SERVER_SESSIONS_H */
