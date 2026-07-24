/* audit_bus.h: the governed-action audit row, carried over the event bus.
 *
 * This is the first REAL module migration onto the shared-memory event bus
 * (delivery step 3). The per-action audit row emitter used to call the file
 * writer (audit_action_log) directly on the caller's thread. It now PUBLISHES the
 * row to the bus, and a dedicated consumer thread drains the bus and performs the
 * real append. The direct call is gone — this is an all-or-nothing transition,
 * not a flagged parallel path: the bus is the sole route for the audit row.
 *
 * Why audit first (memory, the intended hub, is not yet modularized): the audit
 * row is a side-channel, off the answer's critical path, so it is the cheapest
 * place to make the bus load-bearing. A publish is fire-and-forget; the consumer
 * writes asynchronously. The committed budget is therefore an ENQUEUE-overhead
 * ceiling plus a DURABILITY invariant (every accepted row is written exactly
 * once), not a request/reply round-trip.
 *
 * Lifecycle (single process — bus host + consumer live in the server):
 *   audit_bus_start()  once at server startup, after audit_ensure_key / log_init.
 *   audit_bus_emit(..) from any thread, per governed tool call (publish).
 *   audit_bus_stop()   once at shutdown: stops emitting, DRAINS the remaining
 *                      rows, joins the consumer, tears the bus down. The drain is
 *                      what makes shutdown lossless.
 */
#ifndef AIMEE_AUDIT_BUS_H
#define AIMEE_AUDIT_BUS_H 1

#include <stdint.h>

#include "guardrail_events.h" /* guardrail_event_t — a second event kind on this bus */

#ifdef __cplusplus
extern "C"
{
#endif

/* Bus event kinds carried by this in-process observability bus. The bus owns the
 * transport (host, consumer thread, capture stream, retention) and dispatches
 * each recorded off-critical-path event to its own sink:
 *   KIND_ACTION    (3000) — the governed-action audit row  -> the audit ledger
 *   KIND_GUARDRAIL (3001) — the guardrail-semantic risk event -> db1 guardrail_events
 * Shared so writers and the replay reader (audit_replay.c) agree on them. */
#define AUDIT_BUS_KIND_ACTION    3000
#define AUDIT_BUS_KIND_GUARDRAIL 3001

   /* Bring the audit bus up: create the in-process host, attach the producer and
    * the consumer, subscribe the consumer to the audit-row kind, and spawn the
    * consumer thread. Idempotent: a second call while running is a no-op that
    * returns 0. Returns 0 on success, -1 if the bus could not be created. */
   int audit_bus_start(void);

   /* Publish one governed-action audit row. Same field contract as
    * audit_action_log — the fields are serialized and published; the consumer
    * thread performs the real append. Safe to call from multiple threads
    * concurrently (the single producer is serialized internally). If the bus is
    * not running this is a visible no-op (a wiring error, logged), never a silent
    * fallback to a direct write. */
   void audit_bus_emit(const char *actor, const char *tool, const char *args_hash,
                       const char *command, const char *mode, const char *reason_code,
                       const char *verdict, long long task_id);

   /* Publish one guardrail-semantic risk event over the bus (same async,
    * off-critical-path, best-effort contract as audit_bus_emit). The consumer
    * thread performs the real db1 guardrail_events insert; the direct insert at
    * the emit site is gone. Safe to call from any thread; a no-op (logged) if the
    * bus is not running. */
   void audit_bus_emit_guardrail(const guardrail_event_t *e);

   /* Stop emitting, drain every already-published row to the writer, join the
    * consumer thread, and tear the bus down. Lossless: rows published before the
    * call are written before it returns. Idempotent. */
   void audit_bus_stop(void);

   /* Number of rows that could not be published because the bus queue was full
    * (backpressure). Zero under normal load; a non-zero value is a visible signal
    * that the consumer could not keep up, never a silently dropped record. */
   uint64_t audit_bus_dropped(void);

   /* Number of rows the consumer has written to the ledger since start. Exposed
    * for the durability test (published == written + dropped once drained). */
   uint64_t audit_bus_written(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_BUS_H */
