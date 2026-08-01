/* test_server_write_tier_db1.c — the bridge between the write-tier policy and
 * the durable replay store, exercised against a REAL sqlite store.
 *
 * The mapping is security-critical and asymmetric: ONLY an explicit OK counts
 * as fresh. Saturation, a storage fault and a malformed record all mean the jti
 * was not recorded, so treating any of them as "not replayed" would let the
 * same token be presented again for as long as the condition lasts. */
#include "db1.h"
#include "db1_internal.h"
#include "server_write_tier_db1.h"

#include "platform_test_util.h" /* platform_tmpdir */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_managed_identity;

int kb_client_mtls_managed_metadata(char *server_id, size_t cap, long long *team_id)
{
   if (!g_managed_identity)
      return 0;
   snprintf(server_id, cap, "wizard-managed-server");
   *team_id = 23;
   return 1;
}

static server_identity_token_claims_t claims(const char *jti, kb_identity_tier_t tier)
{
   server_identity_token_claims_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "kb");
   snprintf(c.audience, sizeof(c.audience), "server-1");
   snprintf(c.subject, sizeof(c.subject), "oidc:idp.test:user-1");
   snprintf(c.jti, sizeof(c.jti), "%s", jti);
   snprintf(c.kid, sizeof(c.kid), "kid-a");
   c.team_id = 7;
   c.tier = tier;
   c.issued_at = 100;
   c.expires_at = 400;
   return c;
}

int main(void)
{
   unsetenv("AIMEE_SERVER_TEAM_ID");
   unsetenv("AIMEE_SERVER_ID");
   unsetenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_TEAM);
   setenv("AIMEE_SERVER_TEAM_ID", "not-a-team", 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_TEAM);
   setenv("AIMEE_SERVER_TEAM_ID", "7", 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_SERVER_ID);
   setenv("AIMEE_SERVER_ID", "managed-server", 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE);

   /* A path that does not exist is NOT a supplied input. The shipped standalone
    * compose defaults this variable to a conventional location nothing mounts, so
    * treating "set" as READY made a deployment with no authority report ready
    * while every KB-issued token was denied. Only a readable bundle is READY. */
   setenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE", "/run/aimee/management/jwks-trust-bundle.json", 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE);

   char bundle[512];
   snprintf(bundle, sizeof(bundle), "%s/aimee-trust-XXXXXX", platform_tmpdir());
   int bundle_fd = mkstemp(bundle);
   assert(bundle_fd >= 0);
   assert(write(bundle_fd, "{}", 2) == 2);
   close(bundle_fd);
   setenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE", bundle, 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_READY);
   printf("ok: startup preflight identifies each missing Compose input\n");

   unsetenv("AIMEE_SERVER_TEAM_ID");
   unsetenv("AIMEE_SERVER_ID");
   g_managed_identity = 1;
   assert(server_write_tier_team_configured() == 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_READY);
   setenv("AIMEE_SERVER_TEAM_ID", "7", 1);
   assert(server_write_tier_config_state() == SERVER_WRITE_TIER_CONFIG_NO_SERVER_ID);
   unsetenv("AIMEE_SERVER_TEAM_ID");
   g_managed_identity = 0;
   printf("ok: managed identity is a fallback and never fills a partial explicit packet\n");

   char path[] = "/tmp/aimee-write-tier-db1-XXXXXX";
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(path) == 0);

   /* Fresh, then replayed. */
   server_identity_token_claims_t c = claims("id-jti-00000001", KB_IDENTITY_TIER_DATA);
   assert(server_write_tier_replay_db1(NULL, &c, 150) == 0);
   assert(server_write_tier_replay_db1(NULL, &c, 151) == 1);
   printf("ok: first use is fresh, second is a replay\n");

   /* All three tiers round-trip through the store's tier column. */
   server_identity_token_claims_t off = claims("id-jti-00000002", KB_IDENTITY_TIER_OFF);
   server_identity_token_claims_t full = claims("id-jti-00000003", KB_IDENTITY_TIER_FULL);
   assert(server_write_tier_replay_db1(NULL, &off, 150) == 0);
   assert(server_write_tier_replay_db1(NULL, &full, 150) == 0);
   printf("ok: every defined tier is storable\n");

   /* An out-of-range tier is corrupt: denied, and never recorded. */
   server_identity_token_claims_t bogus = claims("id-jti-00000004", (kb_identity_tier_t)99);
   assert(server_write_tier_replay_db1(NULL, &bogus, 150) < 0);
   printf("ok: an unrecognized tier denies rather than being stored\n");

   /* A record the store rejects must deny, not read as fresh. A jti below the
    * store's 8-character floor is refused as INVALID, which must map negative. */
   server_identity_token_claims_t tooshort = claims("short", KB_IDENTITY_TIER_DATA);
   assert(server_write_tier_replay_db1(NULL, &tooshort, 150) < 0);
   printf("ok: a record the store refuses denies rather than reading as fresh\n");

   /* A clock outside the token's window is refused by the store, and must not
    * read as fresh either. */
   server_identity_token_claims_t c2 = claims("id-jti-00000005", KB_IDENTITY_TIER_DATA);
   assert(server_write_tier_replay_db1(NULL, &c2, 401) < 0); /* past expiry */
   assert(server_write_tier_replay_db1(NULL, &c2, 99) < 0);  /* before issuance */
   /* ...and neither attempt consumed it, so a legitimate use still works. */
   assert(server_write_tier_replay_db1(NULL, &c2, 150) == 0);
   printf("ok: an out-of-window clock denies without consuming the token\n");

   assert(server_write_tier_replay_db1(NULL, NULL, 150) < 0);
   printf("ok: NULL claims deny\n");

   db1_shutdown();
   unlink(path);
   printf("  PASS: only an explicit store OK counts as fresh\n");
   return 0;
}
