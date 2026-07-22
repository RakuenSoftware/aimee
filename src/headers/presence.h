/* presence.h: unified-presence registry — one aimee instance shared across
 * every surface (CLI, webchat, telegram, IDE, …).
 *
 * Background: today a "session" is implicitly bound to one client connection
 * (one NDJSON socket / one HTTP stream). The unified-presence proposal
 * (docs/proposals/accepted/aimee-unified-presence.md) generalizes that to:
 *
 *   - A *presence* is the instance, scoped to an owner principal (the local
 *     peer-uid principal, e.g. "uid:1000", in the default local-first case).
 *   - A *session* is durable presence state (transcript, working set, active
 *     workspace) — it lives in DB1, not in a connection.
 *   - An *attachment* is a live connection to a session. Multiple attachments
 *     can share one session: input fans in, the turn stream and presence
 *     events fan out. Closing one attachment does not destroy the session.
 *
 * This module owns the in-process, non-durable side of that model:
 *   1. the attachment registry per session (who is currently attached, and
 *      where async events for that session should be delivered);
 *   2. per-session *turn arbitration* — a turn lock + bounded FIFO queue so a
 *      second surface submitting mid-turn queues or is told PRESENCE_BUSY,
 *      never races;
 *   3. per-workspace *single-writer leases* so two attached surfaces cannot
 *      apply conflicting workspace mutations in one turn;
 *   4. an append-only *presence-event ring* per session (turn_started /
 *      turn_delta / turn_done / busy and routed async events) that
 *      GET /v1/sessions/{id}/events streams live — mirroring the proven
 *      openai_runs_store wait/publish pattern;
 *   5. *outbound routing* — mapping a session-tagged async event (delegate
 *      done, cron/trigger fire, reflection) to the attachments subscribed to
 *      it, dispatched through a delivery callback the server wires to the
 *      gateway's delivery_router (kept behind a function pointer so this core
 *      module stays free of gateway/link dependencies and unit-testable).
 *
 * The durable session set itself (transcript, working set) continues to live
 * in DB1 (src/session_state.c, server_session.c); this module references
 * sessions by id and does not own their persistence.
 *
 * Thread-safety: every function is safe to call concurrently. One global mutex
 * guards the registry; each presence carries its own condition variable for
 * event/turn waiters (mirroring openai_runs_store). Bounded and not durable
 * across restarts. */
#ifndef DEC_PRESENCE_H
#define DEC_PRESENCE_H

#include <stddef.h>
#include <stdint.h>

#include "delivery_target.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Sizing. Bounded, fixed tables — no per-call allocation on the hot path. */
#define PRESENCE_MAX        64 /* concurrent live presences (sessions)     */
#define PRESENCE_ATTACH_MAX 16 /* concurrent attachments per session       */
#define PRESENCE_QUEUE_MAX  8  /* queued turn submits per session          */
#define PRESENCE_LEASE_MAX  8  /* concurrent workspace write leases / sess  */
#define PRESENCE_EVENT_RING 64 /* presence-event ring slots per session     */

#define PRESENCE_SESSION_ID_MAX 128
#define PRESENCE_OWNER_MAX      64
#define PRESENCE_SURFACE_MAX    32 /* "cli" / "webchat" / "telegram" / "acp" …  */
#define PRESENCE_TURN_ID_MAX    64
#define PRESENCE_WORKSPACE_MAX  128
#define PRESENCE_EVENT_NAME_MAX 64
#define PRESENCE_EVENT_DATA_MAX 4096

   /* ---- Event-class subscription mask -------------------------------------
    * An attachment subscribes to a set of presence-event classes. Turn-stream
    * events (started/delta/done/busy) are always delivered to live SSE
    * subscribers; the mask governs *outbound routing* of async events to
    * delivery targets (so a phone opts in to "delegate done" pings without
    * being flooded by every turn delta). */
   typedef enum
   {
      PRESENCE_EV_TURN = 1u << 0,      /* turn_started/turn_delta/turn_done/busy */
      PRESENCE_EV_DELEGATE = 1u << 1,  /* a delegate/run completed               */
      PRESENCE_EV_TRIGGER = 1u << 2,   /* a cron/trigger fired                    */
      PRESENCE_EV_REFLECT = 1u << 3,   /* a reflection/synthesis produced output */
      PRESENCE_EV_GUARDRAIL = 1u << 4, /* a guardrail escalation                  */
      PRESENCE_EV_ALL = 0x1fu,
   } presence_event_class_t;

   /* ---- Turn arbitration result ------------------------------------------- */
   typedef enum
   {
      PRESENCE_TURN_ACQUIRED = 0, /* caller holds the turn; run it, then release */
      PRESENCE_TURN_QUEUED,       /* caller was enqueued (want_queue=1); position
                                   * returned in *out_position; wait then retry  */
      PRESENCE_TURN_BUSY,         /* a turn is in flight and caller did not want
                                   * to queue (or the queue is full); the live
                                   * turn id is returned in out_inflight_turn_id */
      PRESENCE_TURN_ERR,          /* unknown session / bad args                  */
   } presence_turn_result_t;

   /* ---- Event wait result (mirrors openai_runs_store_wait) ----------------- */
   typedef enum
   {
      PRESENCE_WAIT_EVENT = 0, /* an event was copied out; advance and call again */
      PRESENCE_WAIT_TIMEOUT,   /* no event within timeout_ms (emit a heartbeat)   */
      PRESENCE_WAIT_GONE,      /* the presence was torn down                      */
   } presence_wait_t;

   /* ====================================================================== */
   /* Lifecycle                                                              */
   /* ====================================================================== */

   /* Initialize the global registry. Idempotent; safe to call from server
    * startup. Must be called before any other entry point in a fresh process.
    * (Unit tests call it directly.) */
   void presence_init(void);

   /* Tear down every presence, waking all waiters with PRESENCE_WAIT_GONE.
    * Intended for clean shutdown / test teardown. */
   void presence_shutdown(void);

   /* ====================================================================== */
   /* Attachment registry                                                    */
   /* ====================================================================== */

   /* Attach a surface to session_id, creating the presence on first attach.
    * `owner` is the principal the presence is scoped to (must match an
    * existing presence's owner; the first attach fixes it). `surface` is a
    * short label ("cli"/"webchat"/"telegram"/"acp"). `target_spec` is an
    * optional delivery-target spec (delivery_target_parse syntax, e.g.
    * "telegram:-100:42") for outbound routing; pass NULL/"" or "origin" for a
    * live stream that has no durable delivery address. `subscribe_mask` is an
    * OR of presence_event_class_t. `persistent` marks a delivery target that
    * should keep receiving routed async events after the attachment's live
    * stream detaches (e.g. a phone that should still get the "PR is green"
    * ping).
    *
    * On success returns 1 and writes a freshly minted attachment id into
    * out_attach_id[out_n] (stable for the life of the attachment; pass to
    * presence_detach / turn / lease calls). Returns 0 on bad args, owner
    * mismatch, or when the presence/attachment tables are full. */
   int presence_attach(const char *session_id, const char *owner, const char *surface,
                       const char *target_spec, unsigned subscribe_mask, int persistent,
                       char *out_attach_id, size_t out_n);

   /* Detach attachment attach_id from session_id. Releases any workspace leases
    * the attachment held and drops it from the registry. The presence itself
    * survives as long as it has either another attachment or a persistent
    * delivery target; otherwise it is torn down (its event waiters wake GONE).
    * Returns 1 if the attachment existed, 0 otherwise. */
   int presence_detach(const char *session_id, const char *attach_id);

   /* Number of currently-live (non-persistent-only) attachments on a session.
    * 0 if unknown. Used by callers to decide single- vs multi-attach behavior
    * and by tests. */
   int presence_attachment_count(const char *session_id);

   /* Tear down the presence for session_id entirely: drop every attachment
    * (live and persistent routing-only), release its workspace leases, and wake
    * any open event waiters with PRESENCE_WAIT_GONE so their SSE streams end.
    * Called when the durable session is closed (handle_session_close) so the
    * in-process presence does not outlive the session it tracks. Returns 1 if a
    * presence existed, 0 otherwise. */
   int presence_session_close(const char *session_id);

   /* Emit a JSON array of the owner's live presences into out[out_n]
    * (NUL-terminated, truncated if needed): each element is
    * {"session_id","owner","attachments","turn_in_flight","turn_id"}.
    * `owner` may be NULL/"" to list every presence. Always writes valid JSON
    * (at least "[]"). Returns the number of presences listed. */
   int presence_list_json(const char *owner, char *out, size_t out_n);

   /* Emit a JSON object describing one session's attachments into out[out_n]:
    * {"session_id","owner","turn_in_flight","turn_id","attachments":[
    *   {"attach_id","surface","target","subscribe_mask","persistent"} … ]}.
    * Returns 1 if the session exists (valid JSON written), 0 otherwise (writes
    * an error object). */
   int presence_session_json(const char *session_id, char *out, size_t out_n);

   /* Write the owner principal of session_id into out[out_n] (NUL-terminated).
    * Returns 1 if the session exists and has an owner, 0 otherwise (out is set
    * to ""). Used for cross-principal authorization of turn cancellation. */
   int presence_session_owner(const char *session_id, char *out, size_t out_n);

   /* ====================================================================== */
   /* Turn arbitration                                                       */
   /* ====================================================================== */

   /* Try to acquire the per-session turn lock for attach_id.
    *   - No turn in flight  → PRESENCE_TURN_ACQUIRED; a new turn id is written
    *     into out_turn_id[id_n] and must be passed to presence_turn_release.
    *   - Turn in flight, want_queue=1, queue has room → PRESENCE_TURN_QUEUED,
    *     *out_position set to the caller's 1-based queue position.
    *   - Turn in flight, want_queue=0 (or queue full) → PRESENCE_TURN_BUSY,
    *     the in-flight turn id written into out_inflight_turn_id[infl_n].
    * out_turn_id / out_inflight_turn_id / out_position may be NULL if unused.
    * A turn_started presence event is published on acquisition. */
   presence_turn_result_t presence_turn_acquire(const char *session_id, const char *attach_id,
                                                int want_queue, char *out_turn_id, size_t id_n,
                                                char *out_inflight_turn_id, size_t infl_n,
                                                int *out_position);

   /* Blocking acquire: like presence_turn_acquire(want_queue=1), but instead of
    * returning PRESENCE_TURN_QUEUED, take a place in line and wait (FIFO-fair)
    * up to timeout_ms for the turn to become available, then acquire it.
    * Returns PRESENCE_TURN_ACQUIRED (out_turn_id set) once held;
    * PRESENCE_TURN_BUSY on timeout or a full queue (out_inflight_turn_id set to
    * the in-flight turn); PRESENCE_TURN_ERR for unknown session / detached
    * attachment / bad args. timeout_ms <= 0 means do not block (immediate
    * BUSY if not acquirable). The wait must run on a caller/handler thread, not
    * a turn-executing worker thread, or it would block the holder it waits on. */
   presence_turn_result_t presence_turn_acquire_wait(const char *session_id, const char *attach_id,
                                                     int timeout_ms, char *out_turn_id, size_t id_n,
                                                     char *out_inflight_turn_id, size_t infl_n);

   /* Release the turn identified by turn_id. Publishes turn_done, then promotes
    * the head of the queue (if any), waking its waiter. Also releases any
    * workspace leases the turn's holder acquired. No-op if turn_id is not the
    * live turn. Returns 1 on release, 0 otherwise. */
   int presence_turn_release(const char *session_id, const char *turn_id);

   /* True (1) if turn_id is the live in-flight turn for session_id. Lets a
    * queued waiter block on presence_wait and re-check, or a caller verify it
    * still holds the turn before a long step. 0 if not (released/unknown). */
   int presence_turn_is_live(const char *session_id, const char *turn_id);

   /* ====================================================================== */
   /* Workspace single-writer leases                                         */
   /* ====================================================================== */

   /* Acquire the single-writer lease for workspace_id on behalf of attach_id
    * (typically the attachment holding the live turn). Reads need no lease;
    * this gates *mutations*. Returns 1 if the lease was granted (or attach_id
    * already holds it — reentrant), 0 if another attachment holds it (caller
    * must not mutate) or on bad args / table full. */
   int presence_workspace_lease_acquire(const char *session_id, const char *workspace_id,
                                        const char *attach_id);

   /* Release the lease for workspace_id if attach_id holds it. Returns 1 if a
    * lease was released, 0 otherwise. */
   int presence_workspace_lease_release(const char *session_id, const char *workspace_id,
                                        const char *attach_id);

   /* The attach_id currently holding workspace_id's lease, into out[out_n]
    * (empty string if unleased). Returns 1 if held, 0 otherwise. */
   int presence_workspace_lease_holder(const char *session_id, const char *workspace_id, char *out,
                                       size_t out_n);

   /* ====================================================================== */
   /* Presence-event ring + SSE wait/publish (mirrors openai_runs_store)      */
   /* ====================================================================== */

   /* Append a presence event (event_name + data_json, both copied) to the
    * session's ring and broadcast to live waiters. data_json longer than
    * PRESENCE_EVENT_DATA_MAX is truncated. No-op if the session is unknown.
    * `cls` tags the event's class for outbound routing (see
    * presence_route_event); pass PRESENCE_EV_TURN for turn-stream events. */
   void presence_publish(const char *session_id, presence_event_class_t cls, const char *event_name,
                         const char *data_json);

   /* Block up to timeout_ms for an event past *cursor.
    *   PRESENCE_WAIT_EVENT  → event copied into ev[ev_n]/data[data_n], *cursor
    *                          advanced past it; call again for the next.
    *   PRESENCE_WAIT_TIMEOUT → nothing new (emit an SSE heartbeat, loop).
    *   PRESENCE_WAIT_GONE    → the presence was torn down; stop.
    * A fresh subscriber passes *cursor = 0 to start at the oldest retained
    * event. Mirrors openai_runs_store_wait exactly. */
   presence_wait_t presence_wait(const char *session_id, uint64_t *cursor, int timeout_ms, char *ev,
                                 size_t ev_n, char *data, size_t data_n);

   /* Convenience publishers for the turn-stream events (all PRESENCE_EV_TURN).
    * data_json is the event payload object (may be NULL → "{}"). */
   void presence_emit_turn_started(const char *session_id, const char *turn_id);
   void presence_emit_turn_delta(const char *session_id, const char *turn_id,
                                 const char *delta_json);
   void presence_emit_turn_done(const char *session_id, const char *turn_id);

   /* ====================================================================== */
   /* Outbound routing — the presence speaks first                           */
   /* ====================================================================== */

   /* Delivery callback signature: deliver `text` to `target` (already parsed).
    * Returns 0 on success, non-zero on failure. The server wires this to
    * gateway delivery_router_send at startup; unit tests install a stub. */
   typedef int (*presence_delivery_fn)(const delivery_target_t *target, const char *text,
                                       void *user);

   /* Install the delivery callback used by presence_route_event. `user` is
    * passed back verbatim. Passing NULL disables outbound delivery (routed
    * events are still published to the live ring). */
   void presence_set_delivery_fn(presence_delivery_fn fn, void *user);

   /* Route an async event for session_id: publish it to the live ring (so any
    * open SSE stream sees it), then, for every attachment subscribed to `cls`
    * that has a non-origin delivery target, dispatch `text` via the installed
    * delivery callback. Persistent targets receive the dispatch even when their
    * live stream has detached. Returns the number of delivery dispatches made
    * (0 if none subscribed, no callback installed, or session unknown). */
   int presence_route_event(const char *session_id, presence_event_class_t cls,
                            const char *event_name, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* DEC_PRESENCE_H */
