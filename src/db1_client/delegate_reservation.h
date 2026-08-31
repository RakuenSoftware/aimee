/* db1/delegate_reservation.h: execution-key -> delegate job reservations.
 *
 * A workflow step that re-runs must replay the delegate job it already paid
 * for, not launch a second one. The mapping from a step's execution key to its
 * durable agent_jobs row is that reservation.
 *
 * The ledger used to live in the Go control plane, which meant the reservation
 * was read, the job was launched over HTTP, and the reservation was written
 * back -- three round trips with the launch in the middle. A crash in that
 * window left a paid-for job with no reservation, and nothing could replay it.
 * Here the lookup, the launch and the write happen on the side that owns
 * agent_jobs, so a reservation cannot outlive or precede its job.
 *
 * lifecycle_delegate_job is created by the Go control plane's migrations. Every
 * function below tolerates its absence and reports a miss, so a server running
 * without that plane degrades to launching each time rather than failing.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_DELEGATE_RESERVATION_H
#define DEC_DB1_DELEGATE_RESERVATION_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_DELEGATE_RESERVATION_KEY_MAX 512

   /* Resolve an execution key to its reserved job. Returns 0 and fills
    * *out_job_id (>0) plus the participant token on a hit; -1 on a miss, an
    * absent table, or an unusable row. participant may be NULL when the caller
    * only needs the job id; otherwise it is always NUL-terminated. */
   int db1_delegate_reservation_get(const char *execution_key, int *out_job_id, char *participant,
                                    size_t participant_cap);

   /* Adopt a reservation written by an older execution-key algorithm. The new
    * and old keys must share the complete work-item/stage/version prefix, and
    * exactly one usable row may match. The row is moved atomically to
    * execution_key; ambiguous grouped reservations fail closed with -1. */
   int db1_delegate_reservation_adopt_sole_legacy(const char *execution_key,
                                                  const char *work_item_id, int *out_job_id,
                                                  char *participant, size_t participant_cap);

   /* Reserve execution_key for job_id, replacing any previous reservation under
    * that key. work_item_id may be NULL or "" for a reservation that no workflow
    * item owns. Returns 0 on success, -1 on error or an absent table. */
   int db1_delegate_reservation_save(const char *execution_key, const char *work_item_id,
                                     int job_id, const char *participant);

   /* Drop the reservation under execution_key so the next attempt launches
    * fresh. Returns 0 when the key is now unreserved, including when it never
    * was; -1 on error. */
   int db1_delegate_reservation_forget(const char *execution_key);

   /* Drop the reservation only when it still points at job_id. Returns 1 when
    * exactly that reservation was removed, 0 when it was already gone or now
    * names a different job, -1 on error.
    *
    * The compare is what keeps a retry safe: an unconditional delete issued for
    * a job that has since been superseded would erase the newer reservation and
    * let a third launch happen. */
   int db1_delegate_reservation_forget_if_matches(const char *execution_key, int job_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DELEGATE_RESERVATION_H */
