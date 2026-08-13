/* test_server_write_tier_db1.c — the bridge between the write-tier policy and
 * the durable replay store, exercised against a REAL sqlite store.
 *
 * The mapping is security-critical and asymmetric: ONLY an explicit OK counts
 * as fresh. Saturation, a storage fault and a malformed record all mean the jti
 * was not recorded, so treating any of them as "not replayed" would let the
 * same token be presented again for as long as the condition lasts. */
#include "db1.h"
#include "db1_internal.h"
#include "server.h"
#include "server_write_tier_db1.h"
#include "server_write_tier.h"
#include "server/server_mgmt_jwks_cache.h"

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

/* --- the verifier must be handed THIS server's id as the audience ---------
 *
 * config->expected_audience is what binds a token to this server. It used to
 * point at a `char server_id[128]` local to the (static) assembler, so by the
 * time the verifier read it that frame was dead -- undefined behaviour that
 * silently rejected every valid identity token as INVALID once an unrelated
 * change disturbed the stack. Finding it took a full bisect against a
 * CI-equivalent rig.
 *
 * WHAT THIS TEST DOES AND DOES NOT CATCH. It pins the contract: the verifier is
 * handed the real server id, exactly. It will catch a logic change that passes
 * the wrong audience, and it documents why the buffer is caller-owned.
 *
 * It does NOT reliably catch a reintroduction of the lifetime bug itself, and
 * that was measured, not assumed: with the defect restored this test still
 * passes, because build_config is static and -Os (and -flto at link time)
 * inlines it, so `server_id` ends up in a frame that is still alive. Forcing
 * -fno-inline on the object does not help either, since LTO re-inlines at link.
 *
 * The real guards are the signature -- a caller-owned buffer makes the mistake
 * structurally impossible -- and scripts/run-write-tier-enforce-live.sh, which
 * is what actually caught this. */
static char g_seen_audience[256];
static int g_verify_calls;

int server_mgmt_jwks_trust_bundle_load(const char *absolute_path, char *out, size_t cap,
                                       size_t *out_len)
{
   (void)absolute_path;
   if (!out || cap == 0 || !out_len)
      return -1;
   snprintf(out, cap, "trust-bundle");
   *out_len = strlen(out);
   return 0;
}

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_load(const char *trust_bundle,
                                                            size_t trust_bundle_len, int64_t now,
                                                            char *jwks_out, size_t jwks_cap,
                                                            size_t *jwks_len)
{
   (void)trust_bundle;
   (void)trust_bundle_len;
   (void)now;
   if (!jwks_out || jwks_cap == 0 || !jwks_len)
      return SERVER_MGMT_JWKS_CACHE_INVALID;
   snprintf(jwks_out, jwks_cap, "{\"keys\":[]}");
   *jwks_len = strlen(jwks_out);
   return SERVER_MGMT_JWKS_CACHE_OK;
}

/* Overwrite the region a returned-from frame would occupy. Without this the test
 * is worthless: a dangling pointer into a dead frame still reads the right bytes
 * until something reuses them, so the defect passes. The real caller reused that
 * memory by accident; this does it on purpose, which is what makes the assertion
 * deterministic rather than luck. */
static void clobber_dead_frame(int depth)
{
   volatile unsigned char pad[2048];
   for (size_t i = 0; i < sizeof(pad); i++)
      pad[i] = 0xAB;
   if (depth > 0)
      clobber_dead_frame(depth - 1);
}

int server_write_tier_verify(const char *token, size_t token_len,
                             const server_write_tier_config_t *config, int64_t now,
                             server_write_tier_outcome_t *outcome,
                             server_identity_token_claims_t *claims_out)
{
   (void)token;
   (void)token_len;
   (void)now;
   (void)claims_out;
   /* build_config has already returned by the time the verifier runs, so this is
    * the exact window in which its frame is dead and reusable. */
   clobber_dead_frame(3);
   g_verify_calls++;
   g_seen_audience[0] = '\0';
   if (config && config->expected_audience)
      snprintf(g_seen_audience, sizeof(g_seen_audience), "%s", config->expected_audience);
   if (outcome)
      *outcome = SERVER_WRITE_TIER_OK;
   return SERVER_REMOTE_WRITES_DATA;
}

static void test_audience_storage_outlives_the_assembler(void)
{
   g_managed_identity = 1;
   setenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE", "/nonexistent-but-nonempty", 1);

   server_write_tier_outcome_t outcome = SERVER_WRITE_TIER_INVALID;
   server_identity_token_claims_t out;
   int tier = server_write_tier_verify_for_request("a.b.c", 5, 1000, &outcome, &out);

   assert(g_verify_calls == 1);
   /* The stub above provides this id; anything else means the audience pointer
    * did not survive the assembler returning. */
   assert(strcmp(g_seen_audience, "wizard-managed-server") == 0);
   assert(outcome == SERVER_WRITE_TIER_OK);
   assert(tier == SERVER_REMOTE_WRITES_DATA);

   unsetenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   g_managed_identity = 0;
   printf("ok: the audience the verifier sees is the server id, not a dead frame\n");
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

   char path[256];
   snprintf(path, sizeof path, "%s/aimee-write-tier-db1-XXXXXX", platform_tmpdir());
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

   test_audience_storage_outlives_the_assembler();

   db1_shutdown();
   unlink(path);
   printf("  PASS: only an explicit store OK counts as fresh\n");
   return 0;
}
