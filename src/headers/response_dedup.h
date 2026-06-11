/* response_dedup.h: short-window response dedup for buffered ingress (§4).
 *
 * A deliberately narrow v1 cache: it short-circuits a re-sent *identical*
 * buffered, non-streaming, tool-free completion within a small TTL window, so a
 * client that retries the same request under one idempotency key does not pay
 * for the provider call twice. It is NOT a general response cache — eligibility
 * is gated by the caller (explicit idempotency key, no tools, buffered 200) and
 * the key incorporates every behaviour-affecting input the caller can see.
 *
 * Safety: the key MUST include the account/tenant boundary (principal) so one
 * caller can never read another's cached response, plus the resolved model and
 * endpoint and a hash of the exact request body. The store is bounded and
 * TTL'd; a hit records usage_kind=avoided (not spend) with the avoided cost from
 * the original realized row. */
#ifndef DEC_RESPONSE_DEDUP_H
#define DEC_RESPONSE_DEDUP_H 1

#include <stddef.h>

/* Default short window (seconds). */
#define RESPONSE_DEDUP_TTL_SECONDS 5

/* Build a stable dedup key from every behaviour-affecting input the ingress
 * handler can see. principal is the account boundary (empty -> "anon"); body is
 * hashed so the key stays bounded. Writes a NUL-terminated key into out. Pure
 * and deterministic so it is unit-tested directly. */
void response_dedup_key(const char *principal, const char *model, const char *endpoint,
                        const char *idempotency_key, const char *body, char *out, size_t out_cap);

/* Look up a live (unexpired) entry for `key` as of `now` (unix seconds). On a
 * hit, *resp_out is set to a malloc'd copy of the cached response (caller frees)
 * and *cost_out to the avoided cost; returns 1. On a miss returns 0 and leaves
 * the outputs untouched. Thread-safe. */
int response_dedup_get(const char *key, long now, char **resp_out, double *cost_out);

/* Store `resp` (copied) under `key` with `cost`, expiring `ttl_seconds` after
 * `now`. Bounded: when full, an expired or soonest-to-expire slot is evicted.
 * A NULL/empty key or response is ignored. Thread-safe. */
void response_dedup_put(const char *key, const char *resp, double cost, long now, int ttl_seconds);

/* Drop all entries (test helper / shutdown). */
void response_dedup_clear(void);

#endif /* DEC_RESPONSE_DEDUP_H */
