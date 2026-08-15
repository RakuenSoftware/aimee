/* msg_session_key.h: the per-identity session key the economizer's
 * gateway-mutation path is keyed by (proposal economizer-gateway-mutation §2.4).
 *
 * The circuit breaker itself is gone from C. It is state, so it lives in the Go
 * economizer module now, which is what makes it work at all: written on one side
 * of the bus and read on the other, a trip could never fire.
 *
 * What remains is the key derivation, and its property still matters. A key is
 * only ever RESOLVABLE from a per-identity credential — there is no shared
 * "_anonymous" bucket — so one caller's failure can never disable reduction for an
 * unrelated caller (§7 R5). Pure and thread-safe: no state, no lock. */
#ifndef DEC_MSG_SESSION_KEY_H
#define DEC_MSG_SESSION_KEY_H 1

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
    * the attacker's OWN bearer hash.
    *
    * NOTE: `aimee-session-id` is a credential-DERIVED binding token, not an
    * independent secret. There is exactly ONE valid value for a given identity
    * (SHA-256(auth_identity)[0..16)), so a client cannot rotate the header to evade
    * its own disable state or reach a different bucket — any other value is rejected
    * and falls back to the bearer hash. Its only purpose is to group one identity's
    * sessions under a stable key across bearer rotation. */
   msg_session_key_status_t msg_session_key_resolve(const char *hdr_session_id, const char *bearer,
                                                    const char *auth_identity,
                                                    char key[MSG_SESSION_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MSG_SESSION_KEY_H */
