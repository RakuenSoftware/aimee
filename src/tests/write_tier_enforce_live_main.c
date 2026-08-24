/* write_tier_enforce_live_main.c — provision the management trust chain and mint
 * real identity tokens, so a LIVE aimee-server can be asked the one question no
 * other test asks: does a minted token's tier actually gate a real write?
 *
 * WHY THIS EXISTS. Every layer of the per-user /v1 write feature is covered and
 * the composed grant path is verified live, but all of that tests grant
 * ADMINISTRATION — writing a row and auditing it. The feature's PURPOSE is
 * ENFORCEMENT: acceptance §11 says a subject at tier `data` gets `2xx` on
 * memory.store, a subject at tier `off` gets `403`, and both get `2xx` on reads.
 * Nothing exercised that. `identity-mint-e2e` mints a token and validates its
 * structure but never presents it to a running server; `test_server_http.c`
 * covers management-config PARSING, not the gate. The chain
 *
 *     grant row -> minted token(tier) -> HTTP -> resolve_write_tier -> route gate
 *
 * had never once been run end to end, and the last three defects on this branch
 * were all composition defects of exactly that shape.
 *
 * WHY A DRIVER AND NOT A SHELL SCRIPT. The server does not accept a raw JWKS. It
 * reads AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE, a root-owned 0644 file pinning a
 * manifest key, then loads a SIGNED PUBLICATION ENVELOPE from the store and validates
 * it against that bundle. Standing that up means real Ed25519 manifest signing,
 * the real envelope encoder, and a real store row — none of which can be faked
 * from the shell. This driver uses the production functions for every step, so a
 * change that breaks the real publication path breaks this rig too.
 *
 * The RSA token key here stands in for the vault-custodied signing key. That is
 * deliberate and is NOT a gap: key custody is what run-identity-mint-e2e.sh
 * already proves against a real KMS helper. What is unproven, and what this rig
 * exists for, is what the server does with a token once it has one.
 *
 * Usage:
 *   write-tier-enforce-live provision --bundle PATH --key PATH [--no-store]
 *   write-tier-enforce-live mint --key PATH --aud SERVER_ID --team N \
 *                                --sub SUBJECT --tier off|data|full --jti ID \
 *                                [--iat N] [--exp N] [--issuer S]
 *
 * `provision` writes the trust bundle and seeds the store; `mint` prints one compact
 * JWS on stdout. Both exit non-zero with a reason on stderr.
 */

#include "db1_client/db1.h"
#include "kb_mgmt_jwks_publication.h"
#include "kb_mgmt_token_public.h"
#include "kb_mgmt_token_roots_provision.h"
#include "kb_identity_token.h"
#include "server/server_mgmt_jwks_cache.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what)
{
   fprintf(stderr, "write-tier-enforce-live: %s\n", what);
   return 1;
}

/* --- the RSA token-authority key ---------------------------------------- */

/* The kid is DERIVED FROM THE MODULUS (kb_mgmt_token_kid), so a token's `kid`
 * claim and the JWKS entry agree only if both come from the same key. Nothing
 * here may invent a kid — that would make the rig pass while a real deployment,
 * where the two are derived independently, fails. */
#define MODULUS_BITS (KB_MGMT_TOKEN_MODULUS_LEN * 8)

static EVP_PKEY *rsa_generate(void)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   if (ctx && EVP_PKEY_keygen_init(ctx) == 1 &&
       EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, MODULUS_BITS) == 1)
      EVP_PKEY_keygen(ctx, &key);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

static EVP_PKEY *rsa_load(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   EVP_PKEY *key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
   fclose(f);
   return key;
}

/* The modulus as the fixed-width big-endian byte string the JWKS encoder wants.
 * BN_bn2binpad, not BN_bn2bin: a modulus with a leading zero byte would encode
 * short and derive a DIFFERENT kid from the same key. */
static int rsa_modulus(EVP_PKEY *key, uint8_t out[KB_MGMT_TOKEN_MODULUS_LEN])
{
   BIGNUM *n = NULL;
   if (!EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) || !n)
      return -1;
   int rc = BN_bn2binpad(n, out, KB_MGMT_TOKEN_MODULUS_LEN) == KB_MGMT_TOKEN_MODULUS_LEN ? 0 : -1;
   BN_free(n);
   return rc;
}

/* NOTE THE RETURN CONVENTION: kb_identity_token_build treats a NON-ZERO return as
 * success (`if (!signed_ok ...) return SIGN_UNAVAILABLE`), which is the opposite of
 * the 0-means-success convention used nearly everywhere else in this tree. */
static int sign_rs256(void *ctx, const unsigned char *in, size_t in_n, unsigned char *sig,
                      size_t sig_cap, size_t *sig_len)
{
   EVP_PKEY *key = ctx;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   size_t n = sig_cap;
   int ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, key) == 1 &&
            EVP_DigestSign(md, sig, &n, in, in_n) == 1;
   EVP_MD_CTX_free(md);
   if (!ok)
      return 0;
   *sig_len = n;
   return 1;
}

/* --- provision ----------------------------------------------------------- */

typedef struct
{
   const char *envelope;
   size_t envelope_len;
} fetch_ctx_t;

static int fetch_envelope(void *opaque, char *out, size_t cap, size_t *out_n)
{
   fetch_ctx_t *ctx = opaque;
   if (ctx->envelope_len >= cap)
      return -1;
   memcpy(out, ctx->envelope, ctx->envelope_len);
   out[ctx->envelope_len] = '\0';
   *out_n = ctx->envelope_len;
   return 0;
}

/* The trust bundle must be a root-owned regular file with no extra links and no
 * group/world WRITE bits. It is public verification material, so 0644 is the
 * container-compatible shape: UID 1000 can read it but cannot modify it. */
static int write_trust_bundle(const char *path, const char *bundle, size_t bundle_n)
{
   (void)unlink(path);
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
   if (fd < 0)
      return -1;
   int ok = write(fd, bundle, bundle_n) == (ssize_t)bundle_n;
   ok = close(fd) == 0 && ok;
   if (ok && chmod(path, 0644) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

static int cmd_provision(const char *bundle_path, const char *key_path,
                         int64_t now, int seed_store)
{
   EVP_PKEY *token_key = rsa_generate();
   if (!token_key)
      return fail("could not generate the token-authority RSA key");
   FILE *kf = fopen(key_path, "w");
   if (!kf || chmod(key_path, 0600) != 0 ||
       PEM_write_PrivateKey(kf, token_key, NULL, NULL, 0, NULL, NULL) != 1)
   {
      if (kf)
         fclose(kf);
      EVP_PKEY_free(token_key);
      return fail("could not write the token key");
   }
   fclose(kf);

   uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN];
   if (rsa_modulus(token_key, modulus) != 0)
   {
      EVP_PKEY_free(token_key);
      return fail("could not extract the RSA modulus");
   }
   EVP_PKEY_free(token_key);

   /* The Ed25519 manifest key: the root of trust the bundle pins. */
   unsigned char manifest_seed[32], manifest_public[32], publication[32];
   if (RAND_bytes(manifest_seed, sizeof(manifest_seed)) != 1 ||
       RAND_bytes(publication, sizeof(publication)) != 1)
      return fail("no entropy for the manifest key");
   EVP_PKEY *manifest = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, manifest_seed, 32);
   size_t public_n = sizeof(manifest_public);
   if (!manifest || EVP_PKEY_get_raw_public_key(manifest, manifest_public, &public_n) != 1 ||
       public_n != 32)
   {
      EVP_PKEY_free(manifest);
      return fail("could not derive the manifest public key");
   }
   EVP_PKEY_free(manifest);

   char bundle[KB_MGMT_PUBLIC_BUNDLE_MAX];
   size_t bundle_n = 0;
   if (kb_mgmt_public_bundle(modulus, sizeof(modulus), manifest_public, publication, bundle,
                             sizeof(bundle), &bundle_n) != 0)
      return fail("could not build the public trust bundle");

   /* A generous validity window: this rig also mints deliberately-expired TOKENS,
    * and an expired JWKS would deny those for the wrong reason. */
   kb_mgmt_jwks_record_t record;
   if (kb_mgmt_jwks_build_unsigned(modulus, sizeof(modulus), now - 3600, now + 86400, &record) != 0)
      return fail("could not build the unsigned JWKS record");
   unsigned char signature[64];
   char manifest_id[65];
   if (kb_mgmt_jwks_ed25519_sign(manifest_seed, (const unsigned char *)record.payload,
                                 record.payload_len, signature) != 0 ||
       kb_mgmt_manifest_wire_id(manifest_public, manifest_id, sizeof(manifest_id)) != 0 ||
       kb_mgmt_jwks_complete(manifest_public, manifest_id, signature, &record) != 0)
      return fail("could not sign the JWKS publication envelope");
   OPENSSL_cleanse(manifest_seed, sizeof(manifest_seed));

   if (write_trust_bundle(bundle_path, bundle, bundle_n) != 0)
      return fail("could not write the trust bundle 0644");

   /* No open. The store is a separate process reached over the bus, so there is
      nothing here to initialise -- the refresh below either finds the module
      serving or fails saying it could not.

      --no-store skips the seeding entirely. The caller that passes it wants a
      key the server has never heard of; writing this envelope would teach the
      server that key and quietly invert the assertion it is about to make. */
   if (seed_store)
   {
      fetch_ctx_t ctx = {record.envelope, record.envelope_len};
      if (server_mgmt_jwks_cache_refresh(bundle, bundle_n, now, fetch_envelope, &ctx) !=
          SERVER_MGMT_JWKS_CACHE_OK)
         return fail("the store refused the signed JWKS envelope");
   }

   /* Prove the server's own read path accepts what we just wrote, here, rather
    * than letting a bad provision surface later as an opaque INVALID at request
    * time — that failure mode cost this branch an evening already. */
   char loaded_bundle[SERVER_MGMT_JWKS_BUNDLE_MAX], jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t loaded_n = 0, jwks_n = 0;
   if (server_mgmt_jwks_trust_bundle_load(bundle_path, loaded_bundle, sizeof(loaded_bundle),
                                          &loaded_n) != 0)
      return fail("the server's loader rejected the trust bundle we just wrote");
   if (server_mgmt_jwks_cache_load(loaded_bundle, loaded_n, now, jwks, sizeof(jwks), &jwks_n) !=
           SERVER_MGMT_JWKS_CACHE_OK ||
       jwks_n == 0)
      return fail("the server's loader rejected the envelope we just stored");

   char kid[KB_MGMT_TOKEN_KID_MAX + 1];
   if (kb_mgmt_token_kid(modulus, sizeof(modulus), kid, sizeof(kid)) != 0)
      return fail("could not derive the kid");
   printf("kid=%s\n", kid);
   return 0;
}

/* --- mint ---------------------------------------------------------------- */

static int tier_from_name(const char *s, kb_identity_tier_t *out)
{
   if (!strcmp(s, "off"))
      *out = KB_IDENTITY_TIER_OFF;
   else if (!strcmp(s, "data"))
      *out = KB_IDENTITY_TIER_DATA;
   else if (!strcmp(s, "full"))
      *out = KB_IDENTITY_TIER_FULL;
   else
      return -1;
   return 0;
}

static int cmd_mint(const char *key_path, const char *issuer, const char *aud, const char *sub,
                    int64_t team, const char *tier_name, const char *jti, int64_t iat, int64_t exp)
{
   EVP_PKEY *key = rsa_load(key_path);
   if (!key)
      return fail("could not load the token key");
   uint8_t modulus[KB_MGMT_TOKEN_MODULUS_LEN];
   kb_identity_token_claims_t c;
   memset(&c, 0, sizeof(c));
   if (rsa_modulus(key, modulus) != 0 ||
       kb_mgmt_token_kid(modulus, sizeof(modulus), c.kid, sizeof(c.kid)) != 0)
   {
      EVP_PKEY_free(key);
      return fail("could not derive the kid from the token key");
   }
   if (tier_from_name(tier_name, &c.tier) != 0)
   {
      EVP_PKEY_free(key);
      return fail("tier must be off, data or full");
   }
   snprintf(c.issuer, sizeof(c.issuer), "%s", issuer);
   snprintf(c.audience, sizeof(c.audience), "%s", aud);
   snprintf(c.subject, sizeof(c.subject), "%s", sub);
   snprintf(c.jti, sizeof(c.jti), "%s", jti);
   c.team_id = team;
   c.issued_at = iat;
   c.expires_at = exp;

   char jwt[KB_IDENTITY_TOKEN_WIRE_MAX + 1];
   size_t jwt_len = 0;
   kb_identity_token_result_t rc =
       kb_identity_token_build(&c, sign_rs256, key, jwt, sizeof(jwt), &jwt_len);
   EVP_PKEY_free(key);
   if (rc != KB_IDENTITY_TOKEN_OK)
   {
      /* The three refusals have different causes and different fixes; collapsing
       * them into one message sends the reader looking at the claims when the
       * signer is what failed. */
      fprintf(stderr, "write-tier-enforce-live: token build failed: %s\n",
              rc == KB_IDENTITY_TOKEN_INVALID            ? "claims rejected"
              : rc == KB_IDENTITY_TOKEN_SIGN_UNAVAILABLE ? "signer failed"
                                                         : "output buffer too small");
      return 1;
   }
   printf("%.*s\n", (int)jwt_len, jwt);
   return 0;
}

/* --- argv ---------------------------------------------------------------- */

static const char *opt(int argc, char **argv, const char *name, const char *dflt)
{
   for (int i = 2; i + 1 < argc; ++i)
      if (!strcmp(argv[i], name))
         return argv[i + 1];
   return dflt;
}

/* A valueless switch, unlike opt() which takes the next argv slot. */
static int has_flag(int argc, char **argv, const char *name)
{
   for (int i = 2; i < argc; ++i)
      if (!strcmp(argv[i], name))
         return 1;
   return 0;
}

static int64_t opt_i64(int argc, char **argv, const char *name, int64_t dflt)
{
   const char *raw = opt(argc, argv, name, NULL);
   if (!raw || !raw[0])
      return dflt;
   char *end = NULL;
   errno = 0;
   long long v = strtoll(raw, &end, 10);
   if (errno || !end || *end)
      return dflt;
   return (int64_t)v;
}

int main(int argc, char **argv)
{
   if (argc < 2)
      return fail("usage: provision | mint (see the file header)");
   int64_t now = (int64_t)time(NULL);
   if (!strcmp(argv[1], "provision"))
   {
      const char *bundle = opt(argc, argv, "--bundle", NULL);
      const char *key = opt(argc, argv, "--key", NULL);
      if (!bundle || !key)
         return fail("provision needs --bundle and --key");
      int seed_store = !has_flag(argc, argv, "--no-store");
      return cmd_provision(bundle, key, now, seed_store);
   }
   if (!strcmp(argv[1], "mint"))
   {
      const char *key = opt(argc, argv, "--key", NULL);
      const char *aud = opt(argc, argv, "--aud", NULL);
      const char *sub = opt(argc, argv, "--sub", NULL);
      const char *tier = opt(argc, argv, "--tier", NULL);
      const char *jti = opt(argc, argv, "--jti", NULL);
      const char *issuer = opt(argc, argv, "--issuer", "kb");
      int64_t team = opt_i64(argc, argv, "--team", 0);
      int64_t iat = opt_i64(argc, argv, "--iat", now);
      int64_t exp = opt_i64(argc, argv, "--exp", now + 300);
      if (!key || !aud || !sub || !tier || !jti || team <= 0)
         return fail("mint needs --key, --aud, --sub, --tier, --jti and --team");
      return cmd_mint(key, issuer, aud, sub, team, tier, jti, iat, exp);
   }
   return fail("unknown command");
}
