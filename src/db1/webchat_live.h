/* db1/webchat_live.h: the live (in-progress) webchat turn, mirrored to db1 so the
 * browser tails it by polling on a fixed timer instead of reconciling a per-token
 * SSE stream client-side. One row per session: the server overwrites the row with
 * the full current answer text + status as the tmux pane is scraped (server-owned,
 * durable, reconnect-safe); the browser GETs the row when its `rev` advances. */
#ifndef DEC_DB1_WEBCHAT_LIVE_H
#define DEC_DB1_WEBCHAT_LIVE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Upsert the live turn for a session: full current `text` + `status`
    * ("active"/"done"/"error"/"idle"). Bumps the row's monotonic `rev` on every
    * call so a poller can detect "something changed". Returns 0 on success. */
   int db1_webchat_live_set(const char *session_id, const char *turn_id, const char *text,
                            const char *status);

   /* Fetch the live turn for `session_id` IF its rev is greater than `since_rev`
    * (so an unchanged row returns "nothing new"). On a fresh row returns 1 and
    * fills the out-params turn_id, text, status (malloc'd; caller frees) + rev.
    * Returns 0 when
    * there is no newer row (outputs untouched), -1 on error. Pass since_rev<0 to
    * always fetch the current row. */
   int db1_webchat_live_get(const char *session_id, long long since_rev, char **turn_id,
                            char **text, char **status, long long *rev);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WEBCHAT_LIVE_H */
