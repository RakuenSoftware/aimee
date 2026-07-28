/* server_bearer_auth.c: which bearers /v1 accepts, and what it says when it
 * accepts none of them.
 *
 * Owns the set of additionally-enrolled bearers outright — storage, publication,
 * and matching — so the accept decision lives in one place instead of being
 * spread across globals in server_http.c. Its own translation unit because
 * server_http.c sits at the 2500-line ceiling enforced by line-check.
 */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "server.h"
#include "server_http.h"

/* Additional bearers accepted alongside the primary. Pairing a client is
 * additive: it must never revoke the credential other clients are already
 * using. Written only from a /v1 route handler on the single listener thread
 * that also reads it for authorization, so the write is serialized against
 * auth reads and needs no lock — the same contract as g_bearer. */
static char g_bearer_extra[AIMEE_API_BEARER_EXTRA_MAX][256];
static int g_bearer_extra_count = 0;

void server_http_set_bearer_extra(const char *const *bearers, int n)
{
   g_bearer_extra_count = 0;
   if (!bearers || n <= 0)
      return;
   for (int i = 0; i < n && g_bearer_extra_count < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      if (!bearers[i] || !bearers[i][0])
         continue;
      snprintf(g_bearer_extra[g_bearer_extra_count], sizeof(g_bearer_extra[0]), "%s", bearers[i]);
      g_bearer_extra_count++;
   }
}

int server_http_enrolled_bearer_count(void)
{
   return g_bearer_extra_count;
}

/* The live authorization decision: the primary bearer plus everything enrolled. */
int server_http_authorize_enrolled(int is_tcp, const char *bearer_cfg, const char *auth_header,
                                   const char *api_key_header, int has_session_key)
{
   return server_http_authorize_multi(is_tcp, bearer_cfg, (const char *const *)g_bearer_extra,
                                      g_bearer_extra_count, auth_header, api_key_header,
                                      has_session_key);
}

/* Body for an auth rejection on /v1. Split out of handle_conn so the wording is
 * testable — the remediation below is the whole point of the message and had no
 * coverage.
 *
 * A previously-enrolled client that starts failing with 401 has almost always
 * been invalidated by a bearer rotation, not by a typo: rotate_bearer replaces
 * the server-wide bearer wholesale, so every already-paired client breaks at
 * once. The bare "missing or invalid bearer token" gave no way to tell that
 * apart from a bad token, and no way back — recovering meant knowing to read
 * aimee.yaml inside the container. Name the recovery path here, where every
 * client sees it (the thin client echoes this text verbatim). */
const char *server_http_auth_error_body(int az)
{
   if (az == 401)
      return "{\"error\":{\"message\":\"missing or invalid bearer token. If this client was "
             "working before, the server's bearer has been rotated and every paired client must "
             "be re-pointed at the new one: read aimee.api.bearer_token from "
             "<AIMEE_HOME>/aimee.yaml on the server, then `aimee remote set <url> <token>`\","
             "\"type\":\"authentication_error\"}}";
   return "{\"error\":{\"message\":\"this endpoint requires a configured bearer token\","
          "\"type\":\"server_error\"}}";
}

/* As server_http_authorize, but any of |extra| is accepted alongside the primary
 * bearer — the property that lets a second client pair without evicting the
 * first.
 *
 * Every candidate is compared even after one matches: returning early would make
 * the response time depend on which token matched, and on how many are
 * configured, handing an attacker an oracle. server_ct_equal is constant-time
 * per comparison; the loop keeps the COUNT of comparisons constant too. */
int server_http_authorize_multi(int is_tcp, const char *bearer_cfg, const char *const *extra,
                                int extra_count, const char *auth_header,
                                const char *api_key_header, int has_session_key)
{
   int primary =
       server_http_authorize(is_tcp, bearer_cfg, auth_header, api_key_header, has_session_key);
   if (primary != 401 || !extra || extra_count <= 0)
      return primary; /* authorized, or a 503 that extra tokens cannot fix */

   const char *presented_bearer = NULL;
   if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0 && auth_header[7])
      presented_bearer = auth_header + 7;

   int authorized = 0;
   for (int i = 0; i < extra_count; i++)
   {
      if (!extra[i] || !extra[i][0])
         continue;
      if (presented_bearer)
         authorized |= server_ct_equal(presented_bearer, extra[i]);
      if (api_key_header && api_key_header[0])
         authorized |= server_ct_equal(api_key_header, extra[i]);
   }
   return authorized ? 0 : 401;
}
