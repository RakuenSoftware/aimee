/* server_bearer_auth.c: which bearers /v1 accepts, and what it says when it
 * accepts none of them.
 *
 * Owns the set of additionally-enrolled bearers outright — storage, publication,
 * and matching — so the accept decision lives in one place instead of being
 * spread across globals in server_http.c. Its own translation unit because
 * server_http.c sits at the 2500-line ceiling enforced by line-check.
 */
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <openssl/sha.h>

#include "config.h"
#include "modules/db1/remote_client_grant.h"
#include "log.h"
#include "platform_random.h"
#include "runtime_secret.h"
#include "vault_config_bootstrap.h"
#include "server.h"
#include "server_http.h"
#include "server_http_identity.h" /* server_http_bearer_matches */
#include <aimee/core/connection/auth.h>

/* Additional bearers accepted alongside the primary. HTTP connections and
 * dispatch-backed routes run on detached connection workers, so authorization,
 * enrollment and rotation genuinely execute concurrently. One lock protects
 * both this set and the primary bearer stored by server_http.c. */
static char g_bearer_extra[AIMEE_API_BEARER_EXTRA_MAX][256];
static int g_bearer_extra_count = 0;
static pthread_mutex_t g_bearer_lock = PTHREAD_MUTEX_INITIALIZER;
/* The DB claim and config publication form one cross-store transaction. Serialize
 * same-process wizard retries so an UNBOUND reader cannot mistake another
 * worker's not-yet-published claim for crash residue and abandon it. */
static pthread_mutex_t g_first_user_bootstrap_lock = PTHREAD_MUTEX_INITIALIZER;

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
   int count = g_bearer_extra_count;
   pthread_mutex_unlock(&g_bearer_lock);
   return count;
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
   return server_http_authorize_enrolled_request(is_tcp, bearer_cfg, auth_header, api_key_header,
                                                 has_session_key, NULL);
}

int server_http_authorize_enrolled_request(int is_tcp, const char *bearer_cfg,
                                           const char *auth_header, const char *api_key_header,
                                           int has_session_key, int *bootstrap_only)
{
   if (bootstrap_only)
      *bootstrap_only = 0;
   pthread_mutex_lock(&g_bearer_lock);
   /* A two-dimensional char array is not an array of char pointers. Build the
    * pointer view explicitly; casting the storage makes token bytes become
    * addresses and crashes as soon as an enrolled bearer is checked. */
   const char *extra[AIMEE_API_BEARER_EXTRA_MAX];
   for (int i = 0; i < g_bearer_extra_count; i++)
      extra[i] = g_bearer_extra[i];
   int result = server_http_authorize_multi(is_tcp, bearer_cfg, extra, g_bearer_extra_count,
                                            auth_header, api_key_header, has_session_key);
   pthread_mutex_unlock(&g_bearer_lock);
   return result;
}

static int bearer_sha256(const char *bearer, char out[65])
{
   unsigned char digest[SHA256_DIGEST_LENGTH];
   if (!bearer || !bearer[0] || !out ||
       !SHA256((const unsigned char *)bearer, strlen(bearer), digest))
      return -1;
   for (size_t i = 0; i < sizeof(digest); i++)
      snprintf(out + i * 2, 3, "%02x", digest[i]);
   out[64] = '\0';
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

static int configured_bearer_snapshot(char out[][256])
{
   return config_server_api_bearer_extra_snapshot(out, AIMEE_API_BEARER_EXTRA_MAX);
}

static void publish_configured_bearers(void)
{
   char configured[AIMEE_API_BEARER_EXTRA_MAX][256];
   const char *extra[AIMEE_API_BEARER_EXTRA_MAX];
   int count = configured_bearer_snapshot(configured);
   if (count < 0)
      count = 0;
   for (int i = 0; i < count; i++)
      extra[i] = configured[i];
   server_http_set_bearer_extra(extra, count);
}

/* Return the configured cleartext bearer whose digest is |wanted|.  Cleartext
 * remains in the existing protected config because the HTTP authenticator needs
 * it; DB1 stores only the digest used to attach identity/grant metadata. */
static int configured_bearer_for_hash(char configured[][256], int count, const char *wanted,
                                      char *out, size_t out_cap)
{
   if (!configured || count < 0 || !wanted || !out || out_cap < 65)
      return 0;
   for (int i = 0; i < count; i++)
   {
      char digest[65];
      if (bearer_sha256(configured[i], digest) == 0 && server_ct_equal(digest, wanted))
      {
         if (strlen(configured[i]) + 1 > out_cap)
         {
            OPENSSL_cleanse(digest, sizeof(digest));
            return 0;
         }
         snprintf(out, out_cap, "%s", configured[i]);
         OPENSSL_cleanse(digest, sizeof(digest));
         return 1;
      }
      OPENSSL_cleanse(digest, sizeof(digest));
   }
   return 0;
}

static int first_user_bootstrap_locked(const char *principal, char *bearer, size_t bearer_cap)
{
   int result = -1;
   if (bearer && bearer_cap)
      bearer[0] = '\0';
   if (!principal || strncmp(principal, "webuser:", 8) != 0 || !principal[8] || !bearer ||
       bearer_cap < 65)
   {
      aimee_log(LOG_ERROR, "first_user", "bootstrap rejected: caller is not a webchat user");
      return -1;
   }

   char configured[AIMEE_API_BEARER_EXTRA_MAX][256];
   int configured_count = configured_bearer_snapshot(configured);
   int primary_present = runtime_secret_has("AIMEE_API_BEARER_TOKEN");
   if (configured_count < 0 || !primary_present ||
       config_server_api_mtls() <= 0)
   {
      /* Name the specific precondition: all three used to fail identically and
       * silently, which is what made a clean-install failure undiagnosable. */
      aimee_log(LOG_ERROR, "first_user",
                "bootstrap rejected: extras_snapshot=%d primary_bearer=%s mtls=%d",
                configured_count, primary_present ? "present" : "MISSING",
                config_server_api_mtls());
      return -1;
   }

   /* A proposed value is required by the atomic DB1 claim even when this call
    * discovers an earlier pending/bound record. It is never published unless
    * the claim result is NEW. */
   char proposed[65] = "";
   char proposed_hash[65] = "";
   if (platform_random_hex(proposed, 64) != 0 || bearer_sha256(proposed, proposed_hash) != 0)
   {
      aimee_log(LOG_ERROR, "first_user",
                "bootstrap failed: could not generate the enrollment "
                "bearer (RNG or digest failure)");
      goto done;
   }

   db1_remote_client_grant_t grant;
   db1_remote_client_claim_result_t claimed =
       db1_remote_client_claim(principal, proposed_hash, (int64_t)time(NULL), &grant);
   if (claimed == DB1_REMOTE_CLIENT_CLAIM_OWNED_BY_OTHER)
   {
      result = -2;
      goto done;
   }
   if (claimed == DB1_REMOTE_CLIENT_CLAIM_BOUND)
   {
      result = 1;
      goto done;
   }
   if (claimed == DB1_REMOTE_CLIENT_CLAIM_UNBOUND)
   {
      if (configured_bearer_for_hash(configured, configured_count, grant.bearer_sha256, bearer,
                                     bearer_cap))
      {
         result = 0;
         goto done;
      }
      /* A crash may have committed DB1 just before the config write.  That row
       * never authenticated and is safe to abandon; retry once with the newly
       * generated value rather than leaving setup permanently wedged. */
      if (db1_remote_client_abandon(grant.bearer_sha256) != 0)
      {
         aimee_log(LOG_ERROR, "first_user",
                   "bootstrap failed: could not abandon the stale unbound grant in DB1");
         goto done;
      }
      claimed = db1_remote_client_claim(principal, proposed_hash, (int64_t)time(NULL), &grant);
   }
   if (claimed != DB1_REMOTE_CLIENT_CLAIM_NEW || configured_count >= AIMEE_API_BEARER_EXTRA_MAX)
   {
      if (claimed == DB1_REMOTE_CLIENT_CLAIM_NEW)
         (void)db1_remote_client_abandon(proposed_hash);
      aimee_log(LOG_ERROR, "first_user",
                "bootstrap failed: DB1 claim=%d (expected NEW=%d), configured_extras=%d/%d",
                (int)claimed, (int)DB1_REMOTE_CLIENT_CLAIM_NEW, configured_count,
                AIMEE_API_BEARER_EXTRA_MAX);
      goto done;
   }

   char vault_name[96];
   snprintf(vault_name, sizeof(vault_name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", configured_count);
   if (vault_runtime_secret_set(vault_name, proposed) != 0)
   {
      (void)db1_remote_client_abandon(proposed_hash);
      aimee_log(LOG_ERROR, "first_user",
                "bootstrap failed: Vault refused to seal the enrollment bearer as %s", vault_name);
      goto done;
   }
   publish_configured_bearers();
   snprintf(bearer, bearer_cap, "%s", proposed);
   result = 0;

done:
   OPENSSL_cleanse(proposed, sizeof(proposed));
   OPENSSL_cleanse(proposed_hash, sizeof(proposed_hash));
   return result;
}

int server_http_first_user_bootstrap(const char *principal, char *bearer, size_t bearer_cap)
{
   pthread_mutex_lock(&g_first_user_bootstrap_lock);
   int result = first_user_bootstrap_locked(principal, bearer, bearer_cap);
   pthread_mutex_unlock(&g_first_user_bootstrap_lock);
   return result;
}

int server_http_first_user_bind_cert(const char *bearer, const char *cert_serial)
{
   if (!bearer || !bearer[0])
      return 0; /* local UDS/operator issuance is not a wizard enrollment */
   char digest[65] = "";
   if (bearer_sha256(bearer, digest) != 0)
      return -1;
   int rc = db1_remote_client_bind(digest, cert_serial, (int64_t)time(NULL));
   OPENSSL_cleanse(digest, sizeof(digest));
   return rc;
}

int server_http_first_user_cert_tier(const char *cert_serial, char *principal, size_t principal_cap)
{
   return db1_remote_client_tier(cert_serial, principal, principal_cap);
}

int server_http_first_user_apply_cert_grant(int mtls_authenticated, const char *cert_serial,
                                            int *tier, char *principal, size_t principal_cap)
{
   if (principal && principal_cap)
      principal[0] = '\0';
   if (!mtls_authenticated || !cert_serial || !cert_serial[0] || !tier || !principal ||
       principal_cap == 0)
      return 0;
   int granted = server_http_first_user_cert_tier(cert_serial, principal, principal_cap);
   if (granted < 0)
   {
      principal[0] = '\0';
      return -1;
   }
   if (granted > *tier)
      *tier = granted;
   return granted;
}

/* Body for an auth rejection on /v1. Split out of handle_conn so the wording is
 * testable — the remediation below is the whole point of the message and had no
 * coverage.
 *
 * A previously-enrolled client can fail after an explicit revoke-all rotation;
 * the bare "missing or invalid bearer token" gave no recovery path. Name the
 * local, kernel-attested recovery path here, where every client sees it. The
 * bearer itself is Vault-only and must never be recovered from config. */
const char *server_http_auth_error_body(int az)
{
   if (az == 401)
      return "{\"error\":{\"message\":\"missing or invalid bearer token. If this client was "
             "working before, it may have been explicitly revoked by a bearer rotation. On the "
             "server, use the kernel-attested local socket to run `aimee api enable`; then use "
             "the transiently returned token with `aimee remote set <url> <token>`. Bearers are "
             "Vault-only and are never stored in aimee.yaml\","
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

   const char *presented_bearer = aimee_core_bearer_token(auth_header);

   int authorized = server_http_bearer_matches(presented_bearer, bearer_cfg);
   authorized |= server_http_bearer_matches(api_key_header, bearer_cfg);

   /* Do not return after a primary match: compare the same configured set for
    * primary, enrolled and invalid credentials alike. */
   for (int i = 0; extra && i < extra_count; i++)
   {
      if (!extra[i] || !extra[i][0])
         continue;
      authorized |= server_http_bearer_matches(presented_bearer, extra[i]);
      authorized |= server_http_bearer_matches(api_key_header, extra[i]);
   }
   return authorized ? 0 : 401;
}
