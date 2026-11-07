/* shadow_mirror.h -- shadow-traffic publishing to dynamic subscribers.
 *
 * When runtime shadow publishing is explicitly armed, peer aimees SUBSCRIBE
 * (POST /v1/shadow/subscribe {url,bearer}) and this server forwards a
 * fire-and-forget canonical rendering of each admitted inbound completion request to every
 * subscriber's REAL ingress -- a plain POST to the subscriber's /v1 endpoint, tagged
 * X-Aimee-Shadow. The subscriber processes it exactly as it processes live
 * traffic (its own provider call, its own in-process IR shadow); its response is
 * discarded and never affects this server's turn.
 *
 * The point is fidelity: nothing bespoke consumes the shadow. It enters the
 * subscriber through the identical path a real request takes, so what it exercises
 * IS the subscriber's production machinery. And the subscriber set is dynamic --
 * spin up a build under test, it subscribes, done -- with no per-tester config on
 * this side. A subscriber that stops accepting deliveries is self-pruned.
 *
 * The inbound snapshot precedes local profile/policy preparation, so the peer
 * applies its own policy once. Unknown/ignored controls, local-only continuation
 * IDs and unrepresentable native state refuse mirroring; the real request is
 * unaffected. Raw bytes can only leave through translation's semantic replay
 * check. Legacy text requests have their own canonical external renderer.
 * Best-effort: bounded in-flight delivery, failures swallowed, drops counted. */
#ifndef DEC_SHADOW_MIRROR_H
#define DEC_SHADOW_MIRROR_H 1

#include <aimee/ir/aimee_ir.h>

/* 1 if shadow publishing is currently ARMED. Runtime-only, in-memory: always 0 at
 * boot, so an enable can never persist across a restart. */
int shadow_mirror_publish_enabled(void);

/* Arm (on != 0) or disarm shadow publishing at runtime. Disarming also forgets all
 * subscribers. There is no persistent form of this -- a restart boots disarmed. */
void shadow_mirror_set_armed(int on);

/* 1 if `path` is a completion endpoint whose traffic is worth mirroring. */
int shadow_mirror_is_mirrorable_path(const char *path);

/* Register (or refresh) a subscriber: prod will POST each mirrored request to
 * `base_url` + the original path, presenting `bearer`. Idempotent by base_url --
 * a repeat call updates the bearer and clears the failure count (so a periodic
 * re-subscribe rescues a subscriber that was pruned or briefly unreachable).
 * Returns 0 on success, -1 if the subscriber table is full or args are invalid. */
int shadow_mirror_subscribe(const char *base_url, const char *bearer);

/* Remove a subscriber by base_url. Returns 0 if removed, -1 if not present. */
int shadow_mirror_unsubscribe(const char *base_url);

/* Current subscriber count. */
int shadow_mirror_subscriber_count(void);

/* HTTP ingress scopes are thread-owned and never inherited by internal jobs.
 * Capture the actual owner-admitted inbound IR once; delivery occurs only after
 * local preparation succeeds. End releases a pending snapshot on every exit. */
void shadow_mirror_begin_request(const char *path, int is_shadow_inbound);
void shadow_mirror_capture_request(const aimee_request_t *request);
void shadow_mirror_dispatch_admitted(const aimee_request_t *owner);
void shadow_mirror_end_request(void);

/* Observability: mirrors delivered / dropped-at-cap / subscribers pruned. */
long shadow_mirror_sent_count(void);
long shadow_mirror_dropped_count(void);
long shadow_mirror_pruned_count(void);

/* Test-only: clear the subscriber table and counters. */
void shadow_mirror_reset(void);

#endif /* DEC_SHADOW_MIRROR_H */
