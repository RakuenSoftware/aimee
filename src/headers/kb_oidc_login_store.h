/* kb_oidc_login_store.h — custody of in-flight OIDC logins.
 *
 * An authorization-code login spans two separate requests to kb: the redirect
 * that starts it, and the callback the IdP sends the browser back with. The
 * secrets drawn by kb_oidc_login_start have to survive between them, and nowhere
 * else. That is all this is.
 *
 * DELIBERATELY PROCESS-LOCAL. A pending login is worthless after its short TTL
 * and must never be usable twice, so there is nothing here worth persisting or
 * replicating: a kb restart mid-login costs the user one click, while a durable
 * store would mean the code_verifier and nonce outlive the login and land in
 * backups. If kb is ever replicated behind a load balancer, the callback must be
 * routed to the instance that started the login (or this becomes a shared store
 * with a deliberate decision about that exposure) — it is not a silent
 * assumption, it is asserted by the store returning "not found" and the login
 * failing closed.
 *
 * Three properties, in the order they matter:
 *
 *   SINGLE USE — _take removes the login. A replayed callback, even one with the
 *     correct state, finds nothing. This is what stops a callback URL sitting in
 *     browser history or a proxy log from being reusable.
 *   EXPIRING — a login not completed within its TTL is gone. The clock is a
 *     parameter, never read internally, so expiry is testable and there is no
 *     hidden time source.
 *   BOUNDED — a fixed table, so an unauthenticated flood of /login/start cannot
 *     grow kb's memory.
 *
 * Lookup compares the state in constant time against every occupied slot, and
 * always scans them all, so neither the value of a live state nor how many
 * logins are pending is observable through timing.
 */
#ifndef DEC_KB_OIDC_LOGIN_STORE_H
#define DEC_KB_OIDC_LOGIN_STORE_H

#include "kb_oidc_login.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Concurrent in-flight logins. A login lives for seconds (the user's redirect
 * round trip), so this is generous for an interactive kb and small enough that
 * the whole table fits well inside a cache line budget for the constant-time
 * scan. */
#define KB_OIDC_LOGIN_STORE_SLOTS 64

/* Default lifetime of a pending login. Long enough for a real IdP interaction
 * including a password and a second factor, short enough that an abandoned
 * login's secrets do not linger. */
#define KB_OIDC_LOGIN_STORE_TTL_DEFAULT 300

/* Hard ceiling on a caller-supplied TTL, so a configuration mistake cannot pin
 * secrets in memory indefinitely. */
#define KB_OIDC_LOGIN_STORE_TTL_MAX 900

   typedef enum
   {
      KB_OIDC_LOGIN_STORE_OK = 0,
      KB_OIDC_LOGIN_STORE_INVALID,  /* bad argument, or a pending login that was
                                     * never started */
      KB_OIDC_LOGIN_STORE_FULL,     /* every slot holds a live login */
      KB_OIDC_LOGIN_STORE_NOT_FOUND /* unknown, already used, or expired */
   } kb_oidc_login_store_result_t;

   /* Retain `pending` until `now + ttl_seconds`. ttl_seconds <= 0 means
    * KB_OIDC_LOGIN_STORE_TTL_DEFAULT; anything above
    * KB_OIDC_LOGIN_STORE_TTL_MAX is rejected rather than clamped, because
    * silently shortening an operator's stated lifetime would be a surprise.
    *
    * Expired logins are reclaimed first. When no slot is free, this REFUSES with
    * _FULL rather than evicting a live login: a flood that filled the table
    * could otherwise knock out a legitimate user's in-flight login, which is a
    * worse outcome than that flood being unable to start new ones for one TTL. */
   kb_oidc_login_store_result_t kb_oidc_login_store_put(const kb_oidc_login_pending_t *pending,
                                                        int64_t now, int ttl_seconds);

   /* Remove and return the login whose state matches `state`. Single use: a
    * second call with the same state returns _NOT_FOUND. `state` may be any
    * caller-supplied string, including a wrong-length one — that is a
    * _NOT_FOUND, never a match. *out is zeroed unless the result is _OK; the
    * caller owns clearing it afterwards with kb_oidc_login_pending_clear. */
   kb_oidc_login_store_result_t kb_oidc_login_store_take(const char *state, int64_t now,
                                                         kb_oidc_login_pending_t *out);

   /* Drop every login that expired at or before `now`. Called automatically by
    * _put; exposed so a periodic sweep can zero abandoned secrets without
    * waiting for the next login attempt. Returns how many were reclaimed. */
   int kb_oidc_login_store_sweep(int64_t now);

   /* Live (unexpired) logins as of `now`. For diagnostics and tests. */
   int kb_oidc_login_store_count(int64_t now);

   /* Zero the whole table. For process shutdown and test isolation. */
   void kb_oidc_login_store_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_OIDC_LOGIN_STORE_H */
