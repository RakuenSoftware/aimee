/* obs_bus.h: the in-process OBSERVABILITY bus — off-critical-path events carried
 * over the shared-memory event bus to their durable sinks + a capture/replay
 * stream.
 *
 * This began (delivery step 3) as the governed-action audit row's migration off a
 * direct, on-thread file write onto the bus, and has since become the shared
 * transport for several off-path observability event kinds (see the KIND_* list
 * below): governed-action audit rows, guardrail-semantic risk events, and — via
 * server-only bridges that reuse the action kind — vault credential-access and
 * sandbox isolation-degradation audit events. Each migration is all-or-nothing:
 * the bus is the SOLE route for that event, not a flagged parallel path.
 *
 * A publish is fire-and-forget; a dedicated consumer thread drains the bus and
 * performs the real append/insert asynchronously. The committed budget is
 * therefore an ENQUEUE-overhead ceiling plus a DURABILITY invariant (every
 * accepted event reaches its sink exactly once), not a request/reply round-trip.
 *
 * Lifecycle (single process — each trusted daemon owns one host + consumer):
 *   obs_bus_start()  once at daemon startup, after its sinks are configured.
 *   obs_bus_emit(..) from any thread, per governed tool call (publish).
 *   obs_bus_stop()   once at shutdown: stops emitting, DRAINS the remaining
 *                      events, joins the consumer, tears the bus down. The drain
 *                      is what makes shutdown lossless.
 *
 * (Historically named audit_bus; renamed to obs_bus once it carried more than the
 * audit row. The durable audit LEDGER, WORM chain, and replay reader keep their
 * audit_* names — they are genuinely about the audit record, not the transport.)
 */
#ifndef AIMEE_OBS_BUS_H
#define AIMEE_OBS_BUS_H 1

#include <stddef.h> /* size_t */
#include <stdint.h>

#include <aimee/core/event_bus/module_client.h>

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
#define OBS_BUS_KIND_ACTION    3000
#define OBS_BUS_KIND_GUARDRAIL 3001

   /* Optional daemon-owned sink for the server-only guardrail event kind. The
    * shared runtime always carries ACTION events; aimee-server installs this
    * callback for its DB1 guardrail store, while aimee-kb deliberately leaves it
    * unset and therefore has no DB1 link edge. The callback is invoked on the
    * consumer thread and returns 0 only after the event is durably accepted by
    * its sink. */
   typedef int (*obs_bus_guardrail_sink_fn)(const guardrail_event_t *event, void *ctx);

   /* Configure the optional guardrail sink before the bus starts. Passing NULL
    * selects the action-only profile used by aimee-kb. Reconfiguration while the
    * bus is running is refused with -1; otherwise returns 0. */
   int obs_bus_set_guardrail_sink(obs_bus_guardrail_sink_fn sink, void *ctx);

   /* Configure the daemon's authenticated local module endpoint before start.
    * `socket_path` and `policy_dir` must be absolute. The policy directory holds
    * one strict *.grant manifest per installed executable. Both server and KB
    * call this shared entry point; each hosts its own independent bus. */
   int obs_bus_configure_module_runtime(const char *socket_path, const char *policy_dir);
   int obs_bus_configure_daemon_module_runtime(const char *daemon_name,
                                               const char *config_directory);

   /* Bring the audit bus up: create the in-process host, attach the producer and
    * the consumer, subscribe the consumer to the audit-row kind, and spawn the
    * consumer thread. Idempotent: a second call while running is a no-op that
    * returns 0. Returns 0 on success, -1 if the bus could not be created. */
   int obs_bus_start(void);

   /* Invoke a separately shipped process module on this daemon's local bus.
    * This is the shared server/KB bridge into the core module client: AMOD
    * framing, correlation, monotonic deadline enforcement, cancellation, and
    * response validation remain in aimee-core-c. The optional cancellation
    * callback is also combined with daemon shutdown, so stop cannot strand a
    * caller. */
   aimee_module_call_result_t obs_bus_module_call(
       uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
       const void *request_body, uint32_t request_len, void *response_body,
       uint32_t response_capacity, uint32_t *response_len, aimee_module_cancelled_fn cancelled,
       void *cancel_context);

   /* Highest number of module calls that have held a client at the same time.
    *
    * Serialization here is not a throughput matter: while every caller shared
    * one client, a long stage could block the very callback it was waiting on.
    * This high-water mark is how that condition is observable at all -- from
    * outside, a call that is waiting for a client is indistinguishable from one
    * that is doing work. */
   int obs_bus_module_peak_concurrency(void);

   /* Return nonzero only while a live local process is attached and registered
    * to serve event_kind. Intended for daemon readiness sampling; no network I/O
    * is performed. */
   int obs_bus_module_available(uint32_t event_kind);

   /* Publish one governed-action audit row. Same field contract as
    * audit_action_log — the fields are serialized and published; the consumer
    * thread performs the real append. Safe to call from multiple threads
    * concurrently (the single producer is serialized internally). If the bus is
    * not running this is a visible no-op (a wiring error, logged), never a silent
    * fallback to a direct write. */
   void obs_bus_emit(const char *actor, const char *tool, const char *args_hash,
                     const char *command, const char *mode, const char *reason_code,
                     const char *verdict, long long task_id);

   /* Publish one guardrail-semantic risk event over the bus (same async,
    * off-critical-path, best-effort contract as obs_bus_emit). The consumer
    * thread performs the real db1 guardrail_events insert; the direct insert at
    * the emit site is gone. Safe to call from any thread; a no-op (logged) if the
    * bus is not running. */
   void obs_bus_emit_guardrail(const guardrail_event_t *e);

   /* Block until the consumer has written every event emitted so far to its sink
    * (the ledger / db1), WITHOUT stopping the bus. For a caller that emits and then
    * reads the sink synchronously (mainly tests: the write is otherwise async).
    * Bounded (~5s); a no-op if the bus is not running. */
   void obs_bus_flush(void);

   /* Stop emitting, drain every already-published row to the writer, join the
    * consumer thread, and tear the bus down. Lossless: rows published before the
    * call are written before it returns. Idempotent. */
   void obs_bus_stop(void);

   /* Number of rows that could not be published because the bus queue was full
    * (backpressure). Zero under normal load; a non-zero value is a visible signal
    * that the consumer could not keep up, never a silently dropped record. */
   uint64_t obs_bus_dropped(void);

   /* Number of rows the consumer has written to the ledger since start. Exposed
    * for the durability test (published == written + dropped once drained). */
   uint64_t obs_bus_written(void);

   /* Write a PII-safe fingerprint of a (kind, key) identity into `out` (>= 16
    * bytes): "mk:" + the first 6 bytes of SHA-256(kind\x1fkey) in hex. Used by the
    * memory-audit bridges (server + aimee-kb) so an agent-supplied key/kind — which
    * can embed PII, and which the KB's content gates do NOT cover — is NEVER written
    * verbatim to the ledger, while still correlating (same identity -> same handle).
    * One shared implementation so the two bridges cannot diverge. */
   void obs_bus_key_fingerprint(const char *kind, const char *key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_OBS_BUS_H */
