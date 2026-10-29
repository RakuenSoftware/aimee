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

/* Wake the scheduler immediately (non-blocking): call after creating a work item
 * or after a human gate is satisfied so a parked run resumes promptly. */
void wfe_scheduler_notify(void);

#endif /* DEC_WFE_SCHEDULER_H */
