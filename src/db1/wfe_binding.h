/* db1/wfe_binding.h: interactive session <-> work-item binding (S2 primary-as-
 * manager). Single-writer: one binding per session; a second session binding the
 * same work-item is refused. Backend access stays private to src/db1/. */
#ifndef DEC_DB1_WFE_BINDING_H
#define DEC_DB1_WFE_BINDING_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Bind session -> work_item. Idempotent: re-binding the SAME session succeeds
    * (enforce_stage is NOT changed on re-bind -> monotonic per session row).
    * Single-writer: refuse (return -2) if work_item is already bound to a
    * DIFFERENT session. Returns 0 on success, -2 on single-writer conflict, -1 on
    * error / bad args. */
   int db1_wfe_bind(const char *session_id, const char *work_item_id, const char *enforce_stage);

   /* Look up a session's binding. Returns 1 if bound (fills wi_out + stage_out), 0
    * if not bound, -1 on error. Either out buffer may be NULL. */
   int db1_wfe_binding_get(const char *session_id, char *wi_out, size_t wi_n, char *stage_out,
                           size_t stage_n);

   /* Release a session's binding (orphan / rebind). Returns 0 (incl. no-op), -1 on
    * error. */
   int db1_wfe_unbind(const char *session_id);

   /* ---- sliding-window lease (S2 step 6 watchdog) ----
    * The lease tracks freshness so a work-item bound but idle (no MEANINGFUL
    * advance) can be detected. lease_expiry is set forward on bind and renewed on
    * each applied advance; a binding whose lease_expiry has passed is "stale". */

   /* Renew the session's lease: lease_expiry := now + ttl_secs. Called on bind and
    * on each applied advance (meaningful progress) -- NOT on trivial turn traffic.
    * ttl_secs == 0 clears the lease (never stale); a NEGATIVE ttl sets a past expiry
    * (immediately stale -- forces expiry). Returns 0, -1 on error / no binding. */
   int db1_wfe_lease_renew(const char *session_id, int ttl_secs);

   /* Read a session's lease_expiry into `out` ("" if unset). Returns 1 if the
    * binding exists, 0 if not, -1 on error. */
   int db1_wfe_lease_expiry_get(const char *session_id, char *out, size_t n);

   /* Fill `out` (each [80]) with the work-item ids of bindings whose lease has
    * lapsed (lease_expiry set AND < now). Returns the count written (<= max), or -1
    * on error. Detection only -- the caller decides what to do (warn/reclaim). */
   int db1_wfe_lease_stale_work_items(char (*out)[80], int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WFE_BINDING_H */
