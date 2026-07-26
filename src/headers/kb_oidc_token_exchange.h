/* kb_oidc_token_exchange.h — the code-for-id_token exchange, as a pure codec.
 *
 * The third step of the relying-party flow (kb_oidc_login.h starts it,
 * kb_oidc_login_store.h keeps it alive across the redirect): kb presents the
 * authorization code and its PKCE verifier to the IdP's token endpoint and gets
 * an id_token back.
 *
 * Only the CODEC lives here — building the request and reading the response —
 * because that is where a mistake is security-relevant, and it is exactly the
 * part that needs no socket to test. The one-shot HTTPS POST is
 * kb_http_tls_exchange (kb/http/kb_http_client.h); this unit never opens one, so
 * every rule below is checked by a unit test rather than against a live IdP.
 *
 * Two things this deliberately does NOT do:
 *
 *   It does not read the vault. The client secret is a parameter, so its
 *   lifetime belongs to the caller that fetched it and nothing here can outlive
 *   or cache it.
 *
 *   It does not verify the id_token. The response parser only extracts it;
 *   kb_oidc_verify_jwt checks the signature and kb_oidc_login_check_nonce binds
 *   it to the login. A token out of this unit is UNTRUSTED STRING DATA, and the
 *   naming says so.
 */
#ifndef DEC_KB_OIDC_TOKEN_EXCHANGE_H
#define DEC_KB_OIDC_TOKEN_EXCHANGE_H

#include "kb_oidc_login.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* An id_token is a compact JWS and shares the identity token's wire ceiling; a
 * response body larger than this is refused rather than truncated. */
#define KB_OIDC_TOKEN_EXCHANGE_JWT_MAX      4096
#define KB_OIDC_TOKEN_EXCHANGE_BODY_MAX     8192
#define KB_OIDC_TOKEN_EXCHANGE_RESPONSE_MAX (64U * 1024U)

   typedef enum
   {
      KB_OIDC_TOKEN_EXCHANGE_OK = 0,
      KB_OIDC_TOKEN_EXCHANGE_INVALID,   /* bad argument, or an unusable field */
      KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE, /* would not fit the caller's buffer */
      KB_OIDC_TOKEN_EXCHANGE_MALFORMED, /* response is not a token response */
      KB_OIDC_TOKEN_EXCHANGE_DENIED     /* the IdP returned an OAuth error */
   } kb_oidc_token_exchange_result_t;

   /* Build the application/x-www-form-urlencoded token request for `pending` and
    * the authorization code the callback carried.
    *
    * The redirect_uri sent is the one RETAINED IN THE PENDING LOGIN, not a fresh
    * one from configuration: RFC 6749 §4.1.3 requires it to match the
    * authorization request exactly, and re-deriving it would let a configuration
    * change mid-login turn into a silent mismatch.
    *
    * The code_verifier is included; the code_challenge is not (the IdP holds it).
    * `out` is emptied on failure. */
   kb_oidc_token_exchange_result_t
   kb_oidc_token_request_body(const kb_oidc_login_pending_t *pending, const char *code,
                              const char *client_id, char *out, size_t cap);

   /* Build the `Authorization: Basic ...` VALUE for the token request
    * (RFC 6749 §2.3.1: client_id and client_secret are each form-urlencoded
    * before being joined with ':' and base64'd — skipping that step is the classic
    * cause of "works until a secret contains a colon or a plus").
    *
    * Client-secret-basic is used rather than putting the secret in the body,
    * because a body is far more likely to be logged by a proxy than a header
    * that standard tooling redacts. `out` is emptied on failure. */
   kb_oidc_token_exchange_result_t kb_oidc_token_basic_auth(const char *client_id,
                                                            const char *client_secret, char *out,
                                                            size_t cap);

   /* Extract the id_token from a token-endpoint response body.
    *
    * Refuses, rather than best-effort accepting:
    *   - an OAuth error response ("error" present)  -> _DENIED
    *   - no id_token, or one that is not a string   -> _MALFORMED
    *   - a token_type that is not Bearer            -> _MALFORMED
    *   - anything that is not three dot-separated segments -> _MALFORMED
    * The last check is shape only. The token is UNVERIFIED on return: the caller
    * must run kb_oidc_verify_jwt and kb_oidc_login_check_nonce before believing
    * a single claim in it. */
   kb_oidc_token_exchange_result_t kb_oidc_token_response_id_token(const char *body,
                                                                   size_t body_len,
                                                                   char *unverified_id_token_out,
                                                                   size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_OIDC_TOKEN_EXCHANGE_H */
