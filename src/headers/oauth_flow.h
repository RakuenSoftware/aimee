/*
 * oauth_flow.h: OAuth 2.0 PKCE authorization code flow.
 *
 * Layers the full token-exchange flow on top of the PKCE primitives in
 * oauth_pkce.h:
 *
 *   1. oauth_token_store  / oauth_token_load   — db1/secrets wrappers
 *   2. oauth_token_refresh                     — POST grant_type=refresh_token
 *   3. oauth_token_get                         — auto-refreshes if near expiry
 */
#ifndef DEC_OAUTH_FLOW_H
#define DEC_OAUTH_FLOW_H 1

#include <stddef.h>

/* ---- Token storage keys (keyed by MCP client name) ---- */

#define OAUTH_KEY_ACCESS_TOKEN  "oauth.%s.access_token"
#define OAUTH_KEY_REFRESH_TOKEN "oauth.%s.refresh_token"
#define OAUTH_KEY_EXPIRES_AT    "oauth.%s.expires_at"

/* ---- Parsed token response ---- */

typedef struct
{
   char access_token[4096]; /* JWT access tokens (e.g. codex ~2KB) exceed 1KB */
   char refresh_token[1024];
   char token_type[32];
   long expires_in; /* seconds from now; 0 = unknown */
   long expires_at; /* Unix epoch; 0 = unknown */
} oauth_token_response_t;

/* Parse a JSON token response body into |out|.
 * Accepts the standard RFC 6749 §5.1 fields plus our stored expires_at.
 * Returns 0 on success, -1 on parse error or missing access_token. */
int oauth_token_parse_response(const char *json, oauth_token_response_t *out);

/* ---- Secret-store helpers ---- */

/* Persist the tokens from |resp| for the given MCP client name.
 * Returns 0 on success; -1 if any store call fails. */
int oauth_token_store(const char *client_name, const oauth_token_response_t *resp);

/* Load a valid access token for |client_name| into |buf| (size |len|).
 * Returns 0 if a non-expired token is loaded, -1 otherwise. */
int oauth_token_load(const char *client_name, char *buf, size_t len);

/* Remove all stored tokens for |client_name|.
 * Returns 0 on success. */
int oauth_token_remove(const char *client_name);

/* ---- Token refresh (refresh_token grant) ---- */

/* Refresh expired tokens stored for |client_name| using the given |token_endpoint|.
 * Updates the stored tokens on success.
 * Returns 0 on success, -1 on failure (caller should re-authenticate). */
int oauth_token_refresh(const char *client_name, const char *client_id, const char *token_endpoint);

/* ---- High-level accessor ---- */

/* Return a valid access token for |client_name| in |buf| (size |len|).
 * Automatically refreshes if the stored token is within |skew_secs| of expiry.
 * Returns 0 on success, -1 if no valid token is available (caller must
 * trigger a full re-authorization). */
int oauth_token_get(const char *client_name, const char *client_id, const char *token_endpoint,
                    int skew_secs, char *buf, size_t len);

#endif /* DEC_OAUTH_FLOW_H */
