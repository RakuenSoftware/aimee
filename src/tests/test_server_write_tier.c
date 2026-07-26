/* test_server_write_tier.c — the /v1 write-tier gate that replaces the global
 * aimee.api.remote_writes (proposal §4-§5).
 *
 * Every assertion here is a deny-path assertion except one. That is deliberate:
 * this function's whole job is to refuse, and a gate tested mainly on its happy
 * path is a gate that will fail open the first time something unexpected
 * arrives. Each case therefore checks BOTH that the tier is OFF and that the
 * outcome names the specific reason, so a change that starts denying everything
 * for the wrong reason still fails. */
#include "server_write_tier.h"

#include <assert.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <string.h>

#include "kb_identity_token.h"
#include "oauth_pkce.h"
#include "server.h"

static EVP_PKEY *g_key;
static int sign_rs256(void *ctx, const unsigned char *in, size_t in_n, unsigned char *sig,
                      size_t cap, size_t *sig_n)
{
   (void)ctx;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   size_t n = cap;
   int ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, g_key) == 1 &&
            EVP_DigestSign(md, sig, &n, in, in_n) == 1;
   EVP_MD_CTX_free(md);
   if (!ok)
      return 0;
   *sig_n = n;
   return 1;
}

static EVP_PKEY *new_rsa(unsigned bits)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 &&
          EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)bits) == 1 && EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

static void bn_b64url(EVP_PKEY *key, const char *param, char *out, size_t cap)
{
   BIGNUM *bn = NULL;
   assert(EVP_PKEY_get_bn_param(key, param, &bn) == 1 && bn);
   int len = BN_num_bytes(bn);
   unsigned char buf[1024];
   assert(len > 0 && (size_t)len <= sizeof(buf));
   assert(BN_bn2bin(bn, buf) == len);
   assert(oauth_pkce_base64url_encode(buf, (size_t)len, out, cap) == 0);
   BN_free(bn);
}

static void make_jwks(EVP_PKEY *key, const char *kid, char *out, size_t cap)
{
   char n[1024], e[64];
   bn_b64url(key, OSSL_PKEY_PARAM_RSA_N, n, sizeof(n));
   bn_b64url(key, OSSL_PKEY_PARAM_RSA_E, e, sizeof(e));
   int w = snprintf(out, cap,
                    "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"%s\",\"use\":\"sig\","
                    "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}",
                    kid, n, e);
   assert(w > 0 && (size_t)w < cap);
}

/* Replay stub: 0 = fresh, 1 = seen, -1 = store unavailable. */
static int g_replay_answer = 0;
static int g_replay_calls = 0;
static int replay_stub(void *ctx, const server_identity_token_claims_t *claims, int64_t now)
{
   (void)ctx;
   (void)claims;
   (void)now;
   ++g_replay_calls;
   return g_replay_answer;
}

static size_t mint(const char *kid, const char *aud, int64_t team, kb_identity_tier_t tier,
                   const char *jti, int64_t iat, int64_t exp, char *out, size_t cap)
{
   kb_identity_token_claims_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "kb");
   snprintf(c.audience, sizeof(c.audience), "%s", aud);
   snprintf(c.subject, sizeof(c.subject), "oidc:idp.test:user-1");
   c.team_id = team;
   c.tier = tier;
   snprintf(c.jti, sizeof(c.jti), "%s", jti);
   snprintf(c.kid, sizeof(c.kid), "%s", kid);
   c.issued_at = iat;
   c.expires_at = exp;
   size_t n = 0;
   assert(kb_identity_token_build(&c, sign_rs256, NULL, out, cap, &n) == KB_IDENTITY_TOKEN_OK);
   return n;
}

static int fails = 0;

/* Performs the resolve itself. An earlier version of this test passed
 * `resolve(...)` and `oc` as two arguments to a comparison helper, which is
 * unsequenced in C: the outcome was frequently read BEFORE the call wrote it,
 * so every case silently checked the previous case's outcome. Keeping the call
 * inside the helper makes that mistake unrepresentable. */
static void check(const char *tok, size_t n, const server_write_tier_config_t *cfg, int64_t now,
                  int want_tier, server_write_tier_outcome_t want_outcome, const char *name)
{
   server_write_tier_outcome_t got = (server_write_tier_outcome_t)-1;
   int tier = server_write_tier_resolve(tok, n, cfg, now, &got, NULL);
   if (tier != want_tier || got != want_outcome)
   {
      printf("FAIL: %s (tier=%d want=%d, outcome=%s want=%s)\n", name, tier, want_tier,
             server_write_tier_outcome_str(got), server_write_tier_outcome_str(want_outcome));
      fails++;
   }
   else
      printf("ok: %s [%s]\n", name, server_write_tier_outcome_str(got));
}

int main(void)
{
   g_key = new_rsa(2048);
   char jwks[4096];
   make_jwks(g_key, "kid-a", jwks, sizeof(jwks));
   static const int64_t teams[] = {7, 9};

   server_write_tier_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.jwks_json = jwks;
   cfg.expected_issuer = "kb";
   cfg.expected_audience = "server-1";
   cfg.enrolled_teams = teams;
   cfg.enrolled_team_count = 2;
   cfg.replay = replay_stub;

   char tok[KB_IDENTITY_TOKEN_WIRE_MAX + 1];
   size_t n;

   /* The one happy path: a good token yields exactly its claimed tier. */
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_DATA, "jti-00000001", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_DATA, SERVER_WRITE_TIER_OK,
         "a valid data-tier token grants data");

   n = mint("kid-a", "server-1", 9, KB_IDENTITY_TIER_FULL, "jti-00000002", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_FULL, SERVER_WRITE_TIER_OK,
         "a valid full-tier token grants full");

   /* A token that says "off" is a GRANT of off, not an absence of one. It still
    * denies writes, but it must report OK so it is auditable as a decision. */
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_OFF, "jti-00000003", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_OK,
         "an off-tier token is a decision, not an absence");

   /* No credential at all. */
   check(NULL, 0, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_ABSENT,
         "no token denies as absent");
   check("", 0, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_ABSENT,
         "empty token denies as absent");

   /* Garbage and tampering. */
   check("not-a-token", 11, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "garbage denies as invalid");
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_FULL, "jti-00000004", 1000, 1300, tok,
            sizeof(tok));
   tok[n - 1] = (tok[n - 1] == 'A') ? 'B' : 'A'; /* corrupt the signature */
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "a tampered signature denies as invalid");

   /* Wrong audience: a token for another server must not work here. */
   n = mint("kid-a", "server-2", 7, KB_IDENTITY_TIER_FULL, "jti-00000005", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "another server's audience denies");

   /* Expired, and not-yet-valid. */
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_FULL, "jti-00000006", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1301, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "an expired token denies");
   check(tok, n, &cfg, 999, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "a not-yet-issued token denies");

   /* Unknown kid, distinguished from a forgery. */
   n = mint("kid-rotated", "server-1", 7, KB_IDENTITY_TIER_FULL, "jti-00000007", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_UNKNOWN_KID,
         "an unknown kid is reported distinctly");

   /* Cross-team replay: a valid token for a team this server does not serve. */
   g_replay_calls = 0;
   n = mint("kid-a", "server-1", 42, KB_IDENTITY_TIER_FULL, "jti-00000008", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_WRONG_TEAM,
         "a foreign team denies");
   if (g_replay_calls != 0)
   {
      printf("FAIL: a foreign-team token burned a replay slot\n");
      fails++;
   }
   else
      printf("ok: a foreign-team token does not burn a replay slot\n");

   /* A server enrolled for nothing authorizes nothing. */
   server_write_tier_config_t empty = cfg;
   empty.enrolled_teams = NULL;
   empty.enrolled_team_count = 0;
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_FULL, "jti-00000009", 1000, 1300, tok,
            sizeof(tok));
   /* A server with no team configured reports its OWN misconfiguration rather
    * than blaming the token: an operator seeing "wrong_team" across every
    * request would hunt for bad tokens instead of an unset variable. */
   check(tok, n, &empty, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_NO_TEAM_CONFIGURED,
         "a server with no team configured says so, rather than blaming the token");

   /* Replay, and an unavailable replay store. */
   g_replay_answer = 1;
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_FULL, "jti-00000010", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_REPLAY,
         "a replayed jti denies");
   g_replay_answer = -1;
   check(tok, n, &cfg, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_REPLAY_UNAVAILABLE,
         "an unavailable replay store denies rather than assuming fresh");
   g_replay_answer = 0;

   /* Misconfiguration must deny, not crash or default open. */
   server_write_tier_config_t broken = cfg;
   broken.replay = NULL;
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_FULL, "jti-00000011", 1000, 1300, tok,
            sizeof(tok));
   check(tok, n, &broken, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "a config with no replay hook denies");
   check(tok, n, NULL, 1100, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "a NULL config denies");
   check(tok, n, &cfg, -1, SERVER_REMOTE_WRITES_OFF, SERVER_WRITE_TIER_INVALID,
         "a negative clock denies");

   /* The claims are only published on success. */
   server_write_tier_outcome_t oc = (server_write_tier_outcome_t)-1;
   server_identity_token_claims_t claims;
   memset(&claims, 0xAB, sizeof(claims));
   n = mint("kid-a", "server-1", 7, KB_IDENTITY_TIER_DATA, "jti-00000012", 1000, 1300, tok,
            sizeof(tok));
   assert(server_write_tier_resolve(tok, n, &cfg, 1100, &oc, &claims) == SERVER_REMOTE_WRITES_DATA);
   if (claims.team_id != 7 || strcmp(claims.jti, "jti-00000012") ||
       strcmp(claims.subject, "oidc:idp.test:user-1"))
   {
      printf("FAIL: claims were not published on success\n");
      fails++;
   }
   else
      printf("ok: verified claims are published on success\n");
   memset(&claims, 0xAB, sizeof(claims));
   assert(server_write_tier_resolve("garbage", 7, &cfg, 1100, &oc, &claims) ==
          SERVER_REMOTE_WRITES_OFF);
   if (claims.team_id != 0 || claims.jti[0] != 0)
   {
      printf("FAIL: claims were left populated on a denial\n");
      fails++;
   }
   else
      printf("ok: claims are zeroed on denial\n");

   EVP_PKEY_free(g_key);
   printf(fails ? "test_server_write_tier: FAILED\n"
                : "  PASS: server_write_tier denies by default and names every reason\n");
   return fails ? 1 : 0;
}
