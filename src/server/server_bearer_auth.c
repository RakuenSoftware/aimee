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
#include <pthread.h>

#include "config.h"
#include "server.h"
#include "server_http.h"

/* Additional bearers accepted alongside the primary. HTTP connections and
 * dispatch-backed routes run on detached connection workers, so authorization,
 * enrollment and rotation genuinely execute concurrently. One lock protects
 * both this set and the primary bearer stored by server_http.c. */
static char g_bearer_extra[AIMEE_API_BEARER_EXTRA_MAX][256];
static int g_bearer_extra_count = 0;
static pthread_mutex_t g_bearer_lock = PTHREAD_MUTEX_INITIALIZER;

void server_http_set_bearer_extra(const char *const *bearers, int n)
{
   pthread_mutex_lock(&g_bearer_lock);
   memset(g_bearer_extra, 0, sizeof(g_bearer_extra));
   g_bearer_extra_count = 0;
   if (!bearers || n <= 0)
   {
      pthread_mutex_unlock(&g_bearer_lock);
      return;
   }
   for (int i = 0; i < n && g_bearer_extra_count < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      if (!bearers[i] || !bearers[i][0])
         continue;
      snprintf(g_bearer_extra[g_bearer_extra_count], sizeof(g_bearer_extra[0]), "%s", bearers[i]);
      g_bearer_extra_count++;
   }
   pthread_mutex_unlock(&g_bearer_lock);
}

int server_http_enrolled_bearer_count(void)
{
   pthread_mutex_lock(&g_bearer_lock);
   int n = g_bearer_extra_count;
   pthread_mutex_unlock(&g_bearer_lock);
   return n;
}

/* Replace the live primary under the same lock as the enrolled set. Rotation
 * passes revoke_enrolled=1; startup passes 0 because it publishes the extras
 * loaded from config immediately before publishing the primary. */
void server_http_update_primary_bearer(char *live, size_t live_sz, const char *bearer,
                                       int revoke_enrolled)
{
   if (!live || live_sz == 0)
      return;
   pthread_mutex_lock(&g_bearer_lock);
   memset(live, 0, live_sz);
   if (bearer && bearer[0])
      snprintf(live, live_sz, "%s", bearer);
   if (revoke_enrolled)
   {
      memset(g_bearer_extra, 0, sizeof(g_bearer_extra));
      g_bearer_extra_count = 0;
   }
   pthread_mutex_unlock(&g_bearer_lock);
}

/* Take a stable primary snapshot for capability/bootstrap decisions made later
 * in the same request. */
void server_http_primary_bearer_snapshot(const char *live, char *out, size_t out_sz)
{
   if (!out || out_sz == 0)
      return;
   pthread_mutex_lock(&g_bearer_lock);
   snprintf(out, out_sz, "%s", live ? live : "");
   pthread_mutex_unlock(&g_bearer_lock);
}

/* The live authorization decision: the primary bearer plus everything enrolled. */
int server_http_authorize_enrolled(int is_tcp, const char *bearer_cfg, const char *auth_header,
                                   const char *api_key_header, int has_session_key)
{
   pthread_mutex_lock(&g_bearer_lock);
   /* A two-dimensional char array is not an array of char pointers. Build the
    * pointer view explicitly; casting the storage makes token bytes become
    * addresses and crashes as soon as an enrolled bearer is checked. */
   const char *extra[AIMEE_API_BEARER_EXTRA_MAX];
   for (int i = 0; i < g_bearer_extra_count; i++)
      extra[i] = g_bearer_extra[i];
   int rc = server_http_authorize_multi(is_tcp, bearer_cfg, extra, g_bearer_extra_count,
                                        auth_header, api_key_header, has_session_key);
   pthread_mutex_unlock(&g_bearer_lock);
   return rc;
}

/* Body for an auth rejection on /v1. Split out of handle_conn so the wording is
 * testable — the remediation below is the whole point of the message and had no
 * coverage.
 *
 * A previously-enrolled client can fail after an explicit revoke-all rotation;
 * the bare "missing or invalid bearer token" gave no recovery path. Name that
 * path here, where every client sees it. Ordinary additive enrollment does not
 * revoke any existing client. */
const char *server_http_auth_error_body(int az)
{
   if (az == 401)
      return "{\"error\":{\"message\":\"missing or invalid bearer token. If this client was "
             "working before, it may have been explicitly revoked by a bearer rotation. Read "
             "aimee.api.bearer_token from "
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
   int have_bearer = bearer_cfg && bearer_cfg[0];
   if (has_session_key && !have_bearer)
      return 503;
   if (!is_tcp)
      return 0;
   if (!have_bearer)
      return 503; /* extra tokens cannot paper over a missing primary */

   const char *presented_bearer = NULL;
   if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0 && auth_header[7])
      presented_bearer = auth_header + 7;

   int authorized = presented_bearer ? server_ct_equal(presented_bearer, bearer_cfg) : 0;
   if (api_key_header && api_key_header[0])
      authorized |= server_ct_equal(api_key_header, bearer_cfg);

   /* Do not return after a primary match: compare the same configured set for
    * primary, enrolled and invalid credentials alike. */
   for (int i = 0; extra && i < extra_count; i++)
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
