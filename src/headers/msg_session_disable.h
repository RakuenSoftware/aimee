/* msg_session_disable.h: the per-session circuit breaker for the economizer
 * gateway-mutation path (proposal economizer-gateway-mutation §2.4). A bounded,
 * process-local, TTL'd set of session keys whose gateway reduction has been
 * disabled because a mutated request failed upstream. Mutation is only ever
 * attempted for a session whose key is RESOLVABLE from a per-identity credential —
 * there is no shared "_anonymous" bucket, so one caller's failure can never disable
 * reduction for an unrelated caller (§7 R5). Process-local; no cross-process
 * replication in v1. All entry points are thread-safe (one internal mutex). */
#ifndef DEC_MSG_SESSION_DISABLE_H
#define DEC_MSG_SESSION_DISABLE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 16 lowercase hex chars + NUL. The key is SHA-256(...)[0..16). */
#define MSG_SESSION_KEY_LEN 17

   /* How a session key was resolved, so the caller can log appropriately. */
   typedef enum
   {
      MSG_SESSION_KEY_NONE = 0,       /* identity-less: NO key, NO disable state (pristine pass) */
      MSG_SESSION_KEY_RESOLVED,       /* key derived from a validated header or the bearer */
      MSG_SESSION_KEY_BEARER_BAD_HDR, /* key derived from the bearer AFTER a malformed/mismatched
                                       * aimee-session-id header (caller may WARN, IP-rate-limited)
                                       */
   } msg_session_key_status_t;

   /* Resolve the per-identity session key (ordered): a validated `hdr_session_id`
    * (must equal SHA-256(auth_identity)[0..16), 16 lowercase hex), else
    * SHA-256(bearer)[0..16). NULL-first: when auth_identity is NULL/empty the header
    * is never checked (never SHA256(NULL)) — it falls straight to the bearer. A
    * request with neither a validated header nor a bearer resolves to
    * MSG_SESSION_KEY_NONE and MUST be a pristine passthrough (no disable state
    * written). On any resolved status `key` is filled (NUL-terminated, len 16). An
    * attacker holding a valid bearer cannot forge another identity's key: a header
    * that does not match SHA-256(auth_identity) is rejected and the key falls back to
    * the attacker's OWN bearer hash. */
   msg_session_key_status_t msg_session_key_resolve(const char *hdr_session_id, const char *bearer,
                                                    const char *auth_identity,
                                                    char key[MSG_SESSION_KEY_LEN]);

   /* Is `key` currently disabled (present and not expired)? Lazily clears an expired
    * hit. Safe with any NUL-terminated string; a non-resolved key is never disabled. */
   int msg_session_is_disabled(const char *key);

   /* Disable `key` for ttl_ms with a stable static-literal `reason` (stored by
    * pointer). ttl_ms must be > 0 (a non-positive ttl is ignored — the caller is
    * expected to have failed startup validation). Triggers an insert-time sweep when
    * the table is more than half full, and evicts (expired-first, else
    * oldest-inserted) when at capacity. */
   void msg_session_disable(const char *key, int ttl_ms, const char *reason);

   /* Coarse maintenance sweep of expired entries. Internally rate-limited to at most
    * once per 60s of wall-clock, so it is cheap to call on every request. */
   void msg_session_sweep(void);

   /* Introspection / test support. */
   size_t msg_session_count(void); /* live (non-expired) entry count */
   void msg_session_reset(void);   /* clear the whole table + timers (test-only) */

#ifdef __cplusplus
}
#endif

#endif /* DEC_MSG_SESSION_DISABLE_H */
