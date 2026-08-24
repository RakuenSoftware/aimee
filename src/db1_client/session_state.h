/* db1/session_state.h: per-session guardrail and hook scratch state.
 *
 * Backs the session_state_t struct defined in guardrails.h. Replaces the
 * old ~/.config/aimee/session-<sid>.state JSON file — session state is
 * per-user data and belongs in DB1 next to server_sessions.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_SESSION_STATE_H
#define DEC_DB1_SESSION_STATE_H 1

#include "aimee.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Load session_state row (plus child-table rows) into *out. Returns 0
    * on hit, -1 on miss or error. On miss, *out is left untouched — the
    * caller is responsible for zero-initialization and defaults. */
   int db1_session_state_load(const char *sid, session_state_t *out);

   /* Upsert the session_state row and rewrite its child-table contents
    * atomically in a single transaction. Returns 0 on success. */
   int db1_session_state_save(const char *sid, const session_state_t *in);

   /* Delete all rows for sid across scalar + child tables (CASCADE).
    * Returns 0 on success. */
   int db1_session_state_delete(const char *sid);

   /* Returns 1 if a session_state row exists for sid, 0 otherwise. */
   int db1_session_state_exists(const char *sid);

#define DB1_SS_SID_LEN      64
#define DB1_SS_STATE_TS_LEN 32

   typedef struct
   {
      char session_id[DB1_SS_SID_LEN];
      char updated_at[DB1_SS_STATE_TS_LEN];
      int hook_call_count;
   } db1_session_state_summary_t;

   /* List sessions by updated_at DESC, newest first. Returns rows written. */
   int db1_session_state_list(db1_session_state_summary_t *out, int max);

   /* Fetch a single summary row. Returns 0 on hit, -1 on miss or error. */
   int db1_session_state_get_summary(const char *sid, db1_session_state_summary_t *out);

   /* List session ids older than threshold_seconds. Returns ids written. */
   int db1_session_state_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_SID_LEN],
                                      int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_SESSION_STATE_H */
