/* kb_login_throttle.h — brute-force throttle for kb's PRE-AUTH login routes.
 *
 * WHY THIS EXISTS. POST /v1/identity/login/pam authenticates a real host account
 * through pam_unix, and it is reachable with NO bearer token — necessarily so,
 * because it is the surface a caller with no credential uses to get one. Without a
 * throttle that is an unauthenticated, network-reachable password oracle against
 * /etc/shadow: measured on a live host, twelve wrong-password attempts in a row
 * each returned an immediate 401 with no delay, lockout or backoff. The governing
 * proposal lists "brute-force is rate-limited" as an acceptance criterion, so the
 * route was also failing its own checklist.
 *
 * WHAT IT GUARANTEES
 *   - Two independent budgets, per PEER ADDRESS and per USERNAME. The peer budget
 *     stops one host cracking many accounts; the username budget stops many hosts
 *     cracking one account. Either alone is trivially evaded.
 *   - BOUNDED MEMORY. Fixed tables, no allocation, so an attacker cannot grow
 *     kb's heap by varying the username or the source address.
 *   - FAIL CLOSED. Any state it cannot resolve denies rather than allows.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. It does not distinguish a wrong password from
 * an unknown account, a reserved name or an ungrammatical username: every
 * credential rejection costs the same budget and yields the same answer. A
 * throttle that only counted "real" users would itself be an account-enumeration
 * oracle — the caller could tell the two apart by which attempts got throttled.
 *
 * ACCEPTED COST: per-username throttling means an attacker can lock a known
 * account out of PASSWORD login by failing against it deliberately. That is
 * inherent to per-account rate limiting, not an artefact of this design, which is
 * why the lockout is short and self-healing rather than sticky, and why the peer
 * budget carries most of the weight. A deployment that cannot tolerate it should
 * be on OIDC, where kb never sees a password at all.
 */
#ifndef KB_LOGIN_THROTTLE_H
#define KB_LOGIN_THROTTLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Attempts allowed per window before a lockout begins, and the window itself. */
#define KB_LOGIN_THROTTLE_BUDGET     5
#define KB_LOGIN_THROTTLE_WINDOW_SEC 900 /* 15 minutes */
/* First lockout, doubled per further failure, capped. Short enough that a
 * mistyped password is not an outage, long enough that on-line cracking is not
 * viable: at the cap, one attempt per 15 minutes. */
#define KB_LOGIN_THROTTLE_BASE_LOCK_SEC 30
#define KB_LOGIN_THROTTLE_MAX_LOCK_SEC  900

   /* Record the peer address for the request being served. Called from the
    * accept path; pass NULL or "" when it is unknown, which is treated as a
    * single shared bucket rather than as "no limit". */
   void kb_login_throttle_set_peer(const char *peer_ip);

   /* May this attempt proceed? Returns 0 to allow, or a POSITIVE Retry-After in
    * seconds to refuse. Checks the peer budget and the username budget and
    * returns the longer wait of the two. Purely a query — it records nothing, so
    * a throttled attempt does not extend its own lockout. */
   int kb_login_throttle_check(const char *username, int64_t now);

   /* Charge one credential REJECTION against both budgets. Only a failed
    * CREDENTIAL check belongs here. An authenticated caller later refused for
    * want of a grant has proved its identity and must not be throttled for it. */
   void kb_login_throttle_record_failure(const char *username, int64_t now);

   /* Clear both budgets after a successful credential check, so a user who
    * mistypes a few times and then succeeds is not left throttled. */
   void kb_login_throttle_record_success(const char *username);

   /* Test seam: drop all state. */
   void kb_login_throttle_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* KB_LOGIN_THROTTLE_H */
