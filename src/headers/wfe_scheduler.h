/* wfe_scheduler.h: server-owned autonomy scheduler.
 *
 * A background thread that drives active autonomous workflow work items forward
 * (`wfe_autonomy_run`) independent of any client connection — the engine half of
 * "development continues even if the browser tab closes" (built on the
 * server-owned turn lifecycle). It re-fires on an explicit notify (a new work
 * item submitted, or a human gate satisfied) and on a periodic backstop sweep
 * (crash recovery). Per the full-autonomous-development plan (Phase B). */
#ifndef DEC_WFE_SCHEDULER_H
#define DEC_WFE_SCHEDULER_H 1

/* Start the scheduler thread. Idempotent; safe to call once at server_init. */
void wfe_scheduler_init(void);

/* Wake the scheduler immediately (non-blocking): call after creating a work item
 * or after a human gate is satisfied so a parked run resumes promptly. */
void wfe_scheduler_notify(void);

/* Stop the scheduler thread. Idempotent; safe at server_shutdown. */
void wfe_scheduler_shutdown(void);

/* Drive every active autonomous work item one autonomy pass, synchronously, on
 * the calling thread. The background loop calls this; exposed for deterministic
 * testing (and a synchronous-drive option). */
void wfe_scheduler_run_once(void);

#endif /* DEC_WFE_SCHEDULER_H */
