/* test_server_identity_token.c — round-trip + fail-closed tests for the
 * server-side data-plane identity-token verifier (proposal §4). Builds a real
 * kb-signed token with kb_identity_token_build, constructs a JWKS from the
 * signing key's public half, and asserts server_identity_token_verify accepts
 * the good token and rejects every tampering / policy violation. */
#include <assert.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <string.h>

#include "kb_identity_token.h"
#include "kb_caller_token.h"
#include "oauth_pkce.h" /* oauth_pkce_base64url_encode */
#include "server_identity_token.h"

/* --- RS256 signer over the compact signing input --- */
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

/* base64url a BIGNUM's big-endian bytes. */
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

/* Build a one-key JWKS for `key` under `kid`. */
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

static kb_identity_token_claims_t base_claims(void)
{
   kb_identity_token_claims_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "kb");
   snprintf(c.audience, sizeof(c.audience), "server-abc");
   snprintf(c.subject, sizeof(c.subject), "oidc:idp-example:user-42");
   c.team_id = 7;
   c.tier = KB_IDENTITY_TIER_DATA;
   snprintf(c.jti, sizeof(c.jti), "id-jti-00000001");
   snprintf(c.kid, sizeof(c.kid), "kid-2026-a");
   c.issued_at = 1000;
   c.expires_at = 1300;
   return c;
}

int main(void)
{
   g_key = new_rsa(2048);
   char jwks[4096];
   make_jwks(g_key, "kid-2026-a", jwks, sizeof(jwks));

   char jwt[KB_IDENTITY_TOKEN_WIRE_MAX];
   size_t jl = 0;
   kb_identity_token_claims_t c = base_claims();
   assert(kb_identity_token_build(&c, sign_rs256, NULL, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_OK);

   const int64_t NOW = 1200; /* within [iat=1000, exp=1300) */
   server_identity_token_claims_t out;

   /* 1) Happy path: a valid token verifies and the claims come back. */
   assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", NOW, &out) ==
          SERVER_IDENTITY_TOKEN_OK);
   assert(strcmp(out.issuer, "kb") == 0);
   assert(strcmp(out.audience, "server-abc") == 0);
   assert(strcmp(out.subject, "oidc:idp-example:user-42") == 0);
   assert(out.team_id == 7);
   assert(out.tier == KB_IDENTITY_TIER_DATA);
   assert(strcmp(out.jti, "id-jti-00000001") == 0);
   assert(strcmp(out.kid, "kid-2026-a") == 0);
   assert(out.issued_at == 1000 && out.expires_at == 1300);

   /* The KB ingress calls this exact wrapper: signature, typ, issuer, server
    * audience, team, and OIDC subject are all pinned before caller context is
    * accepted. */
   assert(kb_caller_token_verify(jwt, jl, jwks, "server-abc", 7, NOW, &out) ==
          SERVER_IDENTITY_TOKEN_OK);
   assert(strcmp(out.subject, "oidc:idp-example:user-42") == 0);
   assert(kb_caller_token_verify(jwt, jl, jwks, "server-other", 7, NOW, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);
   assert(kb_caller_token_verify(jwt, jl, jwks, "server-abc", 8, NOW, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);

   /* 2) Every tier round-trips. */
   kb_identity_tier_t tiers[] = {KB_IDENTITY_TIER_OFF, KB_IDENTITY_TIER_DATA,
                                 KB_IDENTITY_TIER_FULL};
   for (unsigned i = 0; i < 3; i++)
   {
      c = base_claims();
      c.tier = tiers[i];
      assert(kb_identity_token_build(&c, sign_rs256, NULL, jwt, sizeof(jwt), &jl) ==
             KB_IDENTITY_TOKEN_OK);
      assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", NOW, &out) ==
             SERVER_IDENTITY_TOKEN_OK);
      assert(out.tier == tiers[i]);
   }

   /* Rebuild the canonical good token for the tampering cases. */
   c = base_claims();
   assert(kb_identity_token_build(&c, sign_rs256, NULL, jwt, sizeof(jwt), &jl) ==
          KB_IDENTITY_TOKEN_OK);

   /* 3) Tampered signature: flip the last character. */
   {
      char t[KB_IDENTITY_TOKEN_WIRE_MAX];
      memcpy(t, jwt, jl + 1);
      t[jl - 1] = (t[jl - 1] == 'A') ? 'B' : 'A';
      assert(server_identity_token_verify(t, jl, jwks, "kb", "server-abc", NOW, &out) ==
             SERVER_IDENTITY_TOKEN_INVALID);
   }

   /* 4) Unknown kid: a JWKS whose only key is under a different kid. */
   {
      char other_jwks[4096];
      make_jwks(g_key, "kid-different", other_jwks, sizeof(other_jwks));
      assert(server_identity_token_verify(jwt, jl, other_jwks, "kb", "server-abc", NOW, &out) ==
             SERVER_IDENTITY_TOKEN_UNKNOWN_KID);
   }

   /* 5) Wrong audience / issuer -> denied. */
   assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-XYZ", NOW, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);
   assert(server_identity_token_verify(jwt, jl, jwks, "not-kb", "server-abc", NOW, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);

   /* 6) Expiry window: at/after exp is denied; before iat is denied. */
   assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", 1300, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);
   assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", 999, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);

   /* 7) A signature from a DIFFERENT key is rejected (JWKS pins the signer). */
   {
      EVP_PKEY *good = g_key;
      g_key = new_rsa(2048); /* sign with an impostor key... */
      char imposter[KB_IDENTITY_TOKEN_WIRE_MAX];
      size_t il = 0;
      c = base_claims();
      assert(kb_identity_token_build(&c, sign_rs256, NULL, imposter, sizeof(imposter), &il) ==
             KB_IDENTITY_TOKEN_OK);
      EVP_PKEY_free(g_key);
      g_key = good; /* ...but verify against the real key's JWKS */
      assert(server_identity_token_verify(imposter, il, jwks, "kb", "server-abc", NOW, &out) ==
             SERVER_IDENTITY_TOKEN_INVALID);
   }

   /* 8) A BARE host-account name is accepted: it is the PAM login's subject form,
    * and what kb_identity_token.h documents the `sub` as. This must agree with the
    * subject CHECK in db2/schema.sql — a subject the database admits but the
    * verifier rejects would mint a token the server then refuses as malformed. */
   {
      c = base_claims();
      snprintf(c.subject, sizeof(c.subject), "alice");
      assert(kb_identity_token_build(&c, sign_rs256, NULL, jwt, sizeof(jwt), &jl) ==
             KB_IDENTITY_TOKEN_OK);
      assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", NOW, &out) ==
             SERVER_IDENTITY_TOKEN_OK);
      assert(strcmp(out.subject, "alice") == 0);
      assert(kb_caller_token_verify(jwt, jl, jwks, "server-abc", 7, NOW, &out) ==
             SERVER_IDENTITY_TOKEN_INVALID);

      c = base_claims();
      snprintf(c.subject, sizeof(c.subject), "svc_user-1.2");
      assert(kb_identity_token_build(&c, sign_rs256, NULL, jwt, sizeof(jwt), &jl) ==
             KB_IDENTITY_TOKEN_OK);
      assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", NOW, &out) ==
             SERVER_IDENTITY_TOKEN_OK);
   }

   /* 8b) But a subject matching NO form is still rejected. Note the fixture needs
    * a space: now that a bare username is valid, an unprefixed string is no
    * longer non-conforming by default. */
   {
      const char *bad[] = {"not a name", "-leading-dash", ".leading-dot", "has/slash",
                           "oidc:onlytwoparts"};
      for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
      {
         c = base_claims();
         snprintf(c.subject, sizeof(c.subject), "%s", bad[i]);
         assert(kb_identity_token_build(&c, sign_rs256, NULL, jwt, sizeof(jwt), &jl) ==
                KB_IDENTITY_TOKEN_OK);
         assert(server_identity_token_verify(jwt, jl, jwks, "kb", "server-abc", NOW, &out) ==
                SERVER_IDENTITY_TOKEN_INVALID);
      }
   }

   /* 9) Malformed wire (single segment) is rejected, not crashed. */
   assert(server_identity_token_verify("not-a-jwt", 9, jwks, "kb", "server-abc", NOW, &out) ==
          SERVER_IDENTITY_TOKEN_INVALID);

   EVP_PKEY_free(g_key);
   printf("  PASS: server_identity_token_verify accepts valid tokens, fails closed on tamper/"
          "policy\n");
   return 0;
}
