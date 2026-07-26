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
/* An authorization code is opaque and unbounded by spec. This ceiling is well
 * clear of what real IdPs emit (Okta, Entra and Keycloak are all under 200) and
 * exists so a callback URL cannot be used to push an arbitrary amount of
 * attacker-chosen text through the parser. */
#define KB_OIDC_LOGIN_CODE_MAX 512
/* RFC 6749 §4.1.2.1 error codes are short keywords; enough for the longest
 * ("unsupported_response_type") several times over. */
#define KB_OIDC_LOGIN_IDP_ERR_MAX 64
/* Matches the server_id grammar every identity table CHECKs against. */
#define KB_OIDC_LOGIN_SERVER_MAX 127

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
      /* Which aimee-server this login wants a write token for. Retained because
       * the callback has to file the intent against the SAME server the login
       * named, and because taking it from the callback's query string would let
       * a forged callback redirect a completed login at another server.
       *
       * Caller-supplied at start, and safe to be: naming a server grants
       * nothing. The intent writer looks up (server_id, team_id, subject) in
       * kb_write_tier_grant and requires the registry to carry that server on
       * that team, so an unknown or foreign server is refused there. */
      char target_server_id[KB_OIDC_LOGIN_SERVER_MAX + 1];
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
      KB_OIDC_LOGIN_DISABLED,
      /* The IdP reported a failure instead of returning a code (RFC 6749
       * §4.1.2.1: access_denied, login_required, ...). Distinguished from
       * _INVALID because it is the IdP's answer, not a malformed request, and the
       * two want different handling: one is reported to the user as "the identity
       * provider refused", the other is a bug or an attack. */
      KB_OIDC_LOGIN_IDP_ERROR
   } kb_oidc_login_result_t;

   /* What the IdP's redirect back to kb carried. Holds no secret of kb's own —
    * the code is single-use and worthless without the retained verifier — but the
    * code is still a bearer of sorts, so clear it once exchanged. */
   typedef struct
   {
      char code[KB_OIDC_LOGIN_CODE_MAX + 1];
      char state[KB_OIDC_LOGIN_SECRET_LEN + 1];
      /* Set only on KB_OIDC_LOGIN_IDP_ERROR; the RFC 6749 "error" code. The
       * error_description is deliberately NOT captured: it is attacker-influenced
       * free text whose only use would be echoing it somewhere.
       *
       * `state` IS still populated on this path — see _callback_parse — so the
       * caller can consume the pending login the error belongs to. */
      char idp_error[KB_OIDC_LOGIN_IDP_ERR_MAX + 1];
   } kb_oidc_login_callback_t;

   /* Validate `cfg`. Callers that accept operator input should use this before
    * storing it, so a broken profile is refused at configuration time rather
    * than at somebody's login. */
   int kb_oidc_login_config_valid(const kb_oidc_login_config_t *cfg);

   /* Start a login for a write token on `target_server_id`: draw
    * state/verifier/nonce, retain the target, and write the authorization URL
    * (response_type=code, PKCE S256, state, nonce) to url_out.
    *
    * `target_server_id` must match the server_id grammar the identity tables
    * CHECK; it is refused here rather than at intent time so a malformed one
    * cannot reach the database. On any failure *pending is zeroed and url_out is
    * empty. */
   kb_oidc_login_result_t kb_oidc_login_start(const kb_oidc_login_config_t *cfg,
                                              const char *target_server_id,
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

   /* Parse an OIDC callback's query string (`code`/`state`, or `error`).
    *
    * A DEDICATED parser rather than the generic query_param() used elsewhere in
    * the kb HTTP layer, because that one does strstr(qs, "key=") and would match
    * "state=" inside "?my_state=" or "&oldstate=" — for a filter parameter that
    * is untidy, for the CSRF token it is the bug. This matches only at a key
    * boundary, refuses a repeated key outright (a duplicate is parameter
    * smuggling, and picking either occurrence picks the attacker's), and
    * percent-decodes per application/x-www-form-urlencoded, which is what a
    * redirect query is: "+" decodes to space.
    *
    * Returns:
    *   _OK        code and state both present and well-formed
    *   _IDP_ERROR "error" was present AND a well-formed state came with it;
    *              out->idp_error and out->state are set, out->code is empty
    *   _INVALID   anything else — absent, duplicated, oversized, undecodable, or
    *              a state that is not the exact fixed secret shape
    *
    * A WELL-FORMED STATE IS REQUIRED ON BOTH BRANCHES. RFC 6749 §4.1.2.1 makes it
    * REQUIRED in the error response whenever the authorization request carried one,
    * and this relying party always sends one, so an ?error= without a valid state
    * did not come from a login this kb started. Returning it to the caller on the
    * error path is what allows the pending login to be consumed there too.
    *
    * The state's shape is checked HERE so a malformed one never reaches the
    * store's constant-time scan; a wrong-but-well-formed state still does, and
    * only kb_oidc_login_check_state may judge it. On any non-_OK result the code
    * and state are empty, so a caller that ignores the result cannot proceed on
    * partial data. */
   kb_oidc_login_result_t kb_oidc_login_callback_parse(const char *query_string,
                                                       kb_oidc_login_callback_t *out);

   /* Zero a callback's contents. Safe on NULL. */
   void kb_oidc_login_callback_clear(kb_oidc_login_callback_t *cb);

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
