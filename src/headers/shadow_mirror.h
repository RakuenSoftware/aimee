/* shadow_mirror.h -- shadow-traffic publishing to dynamic subscribers.
 *
 * When aimee.api.shadow_publish_enabled is on, peer aimees SUBSCRIBE
 * (POST /v1/shadow/subscribe {url,bearer}) and this server forwards a
 * fire-and-forget COPY of each inbound completion request to every subscriber's
 * REAL ingress -- a plain POST to the subscriber's /v1 endpoint, tagged
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
 * Best-effort: bounded in-flight delivery, failures swallowed, drops counted. */
#ifndef DEC_SHADOW_MIRROR_H
#define DEC_SHADOW_MIRROR_H 1

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

/* Fire-and-forget: forward a copy of this request to every subscriber's ingress.
 * No-op when publishing is off, `is_shadow_inbound` is nonzero (loop guard), the
 * path is not mirrorable, or there are no subscribers. Copies what it needs and
 * returns immediately; never blocks the caller or touches the real response. */
void shadow_mirror_dispatch(const char *path, const char *body, int body_len,
                            int is_shadow_inbound);

/* Observability: mirrors delivered / dropped-at-cap / subscribers pruned. */
long shadow_mirror_sent_count(void);
long shadow_mirror_dropped_count(void);
long shadow_mirror_pruned_count(void);

/* Test-only: clear the subscriber table and counters. */
void shadow_mirror_reset(void);

#endif /* DEC_SHADOW_MIRROR_H */
