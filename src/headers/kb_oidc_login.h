/* kb_oidc_login.h — aimee-kb as an OIDC RELYING PARTY (proposal
 * per-user-remote-writes-authz.md §3, increment 4a).
 *
 * kb already acts as a resource server: kb_auth_oidc.h verifies a bearer that
 * somebody else obtained. This is the other half — kb conducting the
 * authorization-code flow itself, so a browser can log in and be minted a
 * data-plane identity token. aimee-server delegates the code exchange here and
 * never sees the IdP or a password (§2).
 *
 * This unit is deliberately PURE: no sockets, no database, no clock of its own.
 * It generates the per-login secrets, builds the authorization URL, and decides
 * whether a callback may proceed. The outbound token-endpoint POST and the
 * pending-login store are separate concerns layered on top, so every fail-closed
 * decision below is unit-testable without an IdP.
 *
 * The three things a relying party must get right, and where they live here:
 *
 *   state  — binds the callback to a login THIS kb started (CSRF). Compared in
 *            constant time by kb_oidc_login_check_state.
 *   PKCE   — binds the code to this login, so a stolen code is useless without
 *            the verifier (RFC 7636). The verifier never leaves this process.
 *   nonce  — binds the id_token to this login, so a token minted for a
 *            different request cannot be replayed into it. Checked by
 *            kb_oidc_login_check_nonce AFTER signature verification.
 *
 * All three are independent 43-char base64url secrets from the platform CSPRNG;
 * a generation failure yields no pending login at all, never a weak one. */
#ifndef DEC_KB_OIDC_LOGIN_H
#define DEC_KB_OIDC_LOGIN_H

#include "kb_identity.h"
#include "oauth_pkce.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Each per-login secret is base64url(32 random bytes) = 43 chars, which is also
 * the RFC 7636 minimum verifier length. */
#define KB_OIDC_LOGIN_SECRET_LEN OAUTH_PKCE_VERIFIER_MIN
#define KB_OIDC_LOGIN_URL_MAX    2048
#define KB_OIDC_LOGIN_NONCE_MAX  256

   /* The relying-party profile. kb holds this; aimee-server never does. The
    * client secret is NOT here: it lives in the vault and is read only by the
    * code-exchange step, so nothing in this unit can leak it. */
   typedef struct
   {
      char issuer[256];        /* expected id_token "iss"; required */
      char client_id[256];     /* required */
      char authorize_url[512]; /* IdP authorization endpoint; required */
      /* IdP token endpoint; required. https on the default port only — the kb
       * egress client pins 443, so a URL naming another port would be silently
       * dialled on 443 and is refused instead. */
      char token_url[512];
      char redirect_uri[512]; /* this kb's callback; required */
      char scope[256];        /* "" -> "openid" */
   } kb_oidc_login_config_t;

   /* A login kb has started and not yet completed. Holds secrets: zero it with
    * kb_oidc_login_pending_clear when the login finishes or expires, and never
    * log or return it to a caller. */
   typedef struct
   {
      char state[KB_OIDC_LOGIN_SECRET_LEN + 1];
      char code_verifier[KB_OIDC_LOGIN_SECRET_LEN + 1];
      char nonce[KB_OIDC_LOGIN_SECRET_LEN + 1];
      char redirect_uri[512]; /* retained: the exchange must send the same one */
   } kb_oidc_login_pending_t;

   typedef enum
   {
      KB_OIDC_LOGIN_OK = 0,
      KB_OIDC_LOGIN_INVALID,     /* missing/malformed configuration or argument */
      KB_OIDC_LOGIN_UNAVAILABLE, /* CSPRNG failed, or the URL did not fit */
      KB_OIDC_LOGIN_STATE_MISMATCH,
      KB_OIDC_LOGIN_NONCE_MISMATCH,
      /* No OIDC login front end is configured. A deliberate state, not a
       * failure: a kb may verify bearers without offering a login, and PAM mode
       * leaves the relying-party profile unset entirely. */
      KB_OIDC_LOGIN_DISABLED
   } kb_oidc_login_result_t;

   /* Validate `cfg`. Callers that accept operator input should use this before
    * storing it, so a broken profile is refused at configuration time rather
    * than at somebody's login. */
   int kb_oidc_login_config_valid(const kb_oidc_login_config_t *cfg);

   /* Start a login: draw state/verifier/nonce, and write the authorization URL
    * (response_type=code, PKCE S256, state, nonce) to url_out.
    * On any failure *pending is zeroed and url_out is empty. */
   kb_oidc_login_result_t kb_oidc_login_start(const kb_oidc_login_config_t *cfg,
                                              kb_oidc_login_pending_t *pending, char *url_out,
                                              size_t url_cap);

   /* Compare the callback's `state` against the pending login, in constant time
    * over the fixed secret length. A missing or wrong-length state is a
    * mismatch, never a pass. */
   kb_oidc_login_result_t kb_oidc_login_check_state(const kb_oidc_login_pending_t *pending,
                                                    const char *state_from_callback);

   /* Compare a VERIFIED id_token's nonce claim against the pending login. Call
    * only after kb_oidc_verify_jwt has accepted the token: on an unverified
    * token the claim is attacker-chosen. Absent claim -> mismatch. */
   kb_oidc_login_result_t kb_oidc_login_check_nonce(const kb_oidc_login_pending_t *pending,
                                                    const char *verified_id_token);

   /* Build the actor principal for a verified id_token result. The identity key
    * is issuer-scoped (`oidc:<iss>:<sub>`) and the issuer comes from the
    * CONFIGURED profile, never from the token — a token cannot nominate its own
    * issuer namespace. Returns KB_OIDC_LOGIN_OK and sets out->authenticated. */
   kb_oidc_login_result_t kb_oidc_login_principal(const kb_oidc_login_config_t *cfg,
                                                  const kb_verify_result_t *verified,
                                                  kb_principal_t *out);

   /* Split an https URL into the host and origin-path the kb egress client takes
    * (kb_http_request_t carries them separately and pins port 443).
    *
    * Refuses, rather than coercing: a non-https scheme; userinfo (an "@" before
    * the path, which is how a URL is made to look like one host while resolving
    * to another); an explicit port — even ":443" — because the client dials 443
    * regardless and accepting a port would mean a URL naming :8443 was silently
    * sent to 443; a query or fragment, which an origin-form target may not carry;
    * and an empty host.
    *
    * An absent path becomes "/". Pass NULL/0 for both outputs to validate only.
    * On failure both outputs are emptied. */
   kb_oidc_login_result_t kb_oidc_token_url_split(const char *url, char *host_out, size_t host_cap,
                                                  char *path_out, size_t path_cap);

   /* Zero a pending login's secrets. Safe on NULL. */
   void kb_oidc_login_pending_clear(kb_oidc_login_pending_t *pending);

   /* Load the relying-party profile from the environment, alongside the verifier
    * configuration kb_oidc_register_from_env already reads (proposal §3 grounds
    * the OIDC mode in that same AIMEE_KB_OIDC_* configuration):
    *
    *   AIMEE_KB_OIDC_LOGIN_CLIENT_ID     required — enables the login front end
    *   AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL required
    *   AIMEE_KB_OIDC_LOGIN_TOKEN_URL     required
    *   AIMEE_KB_OIDC_LOGIN_REDIRECT_URI  required
    *   AIMEE_KB_OIDC_LOGIN_SCOPE         optional; "" -> "openid"
    *   AIMEE_KB_OIDC_ISSUER              required — SHARED with the verifier, so
    *                                     the issuer a login trusts and the issuer
    *                                     a bearer is verified against cannot
    *                                     drift apart
    *
    * The client SECRET is deliberately absent: it is vault-custodied and read
    * only at the moment of the code exchange, so it never sits in kb's
    * environment where a crash dump or a `ps` would reach it.
    *
    * Returns KB_OIDC_LOGIN_OK with *out filled when the login front end is
    * configured; KB_OIDC_LOGIN_DISABLED when AIMEE_KB_OIDC_LOGIN_CLIENT_ID is
    * unset (the deliberate "OIDC login off" state, not an error — a kb may still
    * verify bearers, and PAM mode leaves all of this unset);
    * KB_OIDC_LOGIN_INVALID when it is set but the profile is incomplete or
    * unusable, which must be loud rather than a silent fallback to disabled. */
   kb_oidc_login_result_t kb_oidc_login_config_from_env(kb_oidc_login_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_OIDC_LOGIN_H */
