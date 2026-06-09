/*
 * oauth_pkce.h: PKCE (RFC 7636) primitives for OAuth 2.0 authorization.
 *
 * Provides the foundational building blocks needed by an MCP client to
 * authenticate against OAuth-protected remote servers:
 *
 *   1. code_verifier generation (cryptographic random, unreserved charset)
 *   2. S256 code_challenge derivation (SHA-256 + base64url-no-padding)
 *   3. base64url-no-padding encoding (RFC 4648 §5)
 *   4. authorization URL construction with PKCE parameters
 *
 * Network transport, token exchange, and token storage live in follow-up
 * modules. Keeping the crypto/URL pieces standalone makes them trivial to
 * unit-test and reuse across the CLI and webchat auth flows.
 */
#ifndef DEC_OAUTH_PKCE_H
#define DEC_OAUTH_PKCE_H 1

#include <stddef.h>

/* RFC 7636 §4.1: verifier is 43..128 characters from the unreserved set. */
#define OAUTH_PKCE_VERIFIER_MIN 43
#define OAUTH_PKCE_VERIFIER_MAX 128

/* S256 challenge is SHA-256 (32 bytes) base64url-encoded without padding
 * = 43 characters. Callers should size buffers as OAUTH_PKCE_CHALLENGE_LEN + 1. */
#define OAUTH_PKCE_CHALLENGE_LEN 43

/* Base64url-encode (RFC 4648 §5) |inlen| bytes from |in| into |out|.
 * No padding is emitted. |outlen| must be at least 4*ceil(inlen/3) - pad + 1.
 * Returns 0 on success, -1 if |out| is too small or args are invalid. */
int oauth_pkce_base64url_encode(const unsigned char *in, size_t inlen, char *out, size_t outlen);

/* Fill |out| with a PKCE code_verifier of exactly |len| characters
 * (43..128). |out| must have room for |len| + 1 bytes (NUL terminator).
 * Returns 0 on success, -1 on error (bad args or RNG failure). */
int oauth_pkce_generate_verifier(char *out, size_t len);

/* Compute the S256 code_challenge for |verifier|:
 *   challenge = base64url_no_pad(SHA256(verifier))
 * |out| must have room for OAUTH_PKCE_CHALLENGE_LEN + 1 bytes.
 * Returns 0 on success, -1 on error. */
int oauth_pkce_s256_challenge(const char *verifier, char *out, size_t outlen);

/* Parameters required to build a PKCE-flavored authorization URL.
 * All non-NULL string pointers must be NUL-terminated. */
typedef struct
{
   const char *authorize_url;  /* e.g. "https://example.com/oauth/authorize" */
   const char *client_id;      /* required */
   const char *redirect_uri;   /* required */
   const char *scope;          /* optional; NULL or "" to omit */
   const char *state;          /* optional; NULL or "" to omit */
   const char *code_challenge; /* required, from oauth_pkce_s256_challenge() */
} oauth_pkce_auth_request_t;

/* Write the authorization URL to |out|. The URL has the form
 *   <authorize_url>?response_type=code&client_id=...&redirect_uri=...
 *     &code_challenge=...&code_challenge_method=S256
 *     [&scope=...][&state=...]
 * with query-parameter values percent-encoded per RFC 3986.
 *
 * Returns the number of bytes written (excluding NUL) on success,
 * or -1 if |out| is too small or required fields are missing. */
int oauth_pkce_build_auth_url(const oauth_pkce_auth_request_t *req, char *out, size_t outlen);

#endif /* DEC_OAUTH_PKCE_H */
