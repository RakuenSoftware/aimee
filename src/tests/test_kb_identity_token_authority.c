/* test_kb_identity_token_authority.c — round-trip + fail-closed tests for the
 * protected identity-token mint (proposal per-user-remote-writes-authz.md §4,
 * slice B1).
 *
 * The round trip is deliberately end-to-end across the process boundary this
 * feature actually has: the authority mints with a real vault-shaped RSA-3072
 * key from an admitted record, and the SERVER's verifier
 * (server_identity_token_verify) — the code that will gate /v1 writes — must
 * accept it against a JWKS carrying that key. A mint the server would reject is
 * a silent lockout, so asserting only "a JWT came out" would not be a test. */
#include "management_token_authority.h"

#include <assert.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <string.h>

#include "oauth_pkce.h" /* oauth_pkce_base64url_encode */
#include "server_identity_token.h"

#define ISSUER   "https://kb.example.test"
#define AUDIENCE "server-1"
#define SUBJECT  "oidc:https%3A//idp.example.test:user-42"
#define JTI      "id-jti-00000001"

static int contract_result;

static int management_record_valid_unused(const kb_mgmt_token_authority_record_t *record)
{
   (void)record;
   return 1;
}

static int identity_record_valid_contract(const kb_identity_token_authority_record_t *record)
{
   assert(record);
   return contract_result;
}

static void test_injected_record_validator(void)
{
   kb_identity_token_authority_record_t record = {0};

   aimee_db2_register_token_record_validators(management_record_valid_unused, NULL);
   assert(!db2_management_identity_authority_record_validate(&record));

   aimee_db2_register_token_record_validators(management_record_valid_unused,
                                              identity_record_valid_contract);
   contract_result = 1;
   assert(db2_management_identity_authority_record_validate(&record));
   contract_result = 0;
   assert(!db2_management_identity_authority_record_validate(&record));
   contract_result = 2;
   assert(!db2_management_identity_authority_record_validate(&record));
   contract_result = -1;
   assert(!db2_management_identity_authority_record_validate(&record));
}

static void sha256(const void *data, size_t len, unsigned char out[32])
{
   unsigned int written = 0;
   assert(EVP_Digest(data, len, out, &written, EVP_sha256(), NULL) == 1);
   assert(written == 32);
}

static EVP_PKEY *rsa3072(void)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   assert(ctx);
   assert(EVP_PKEY_keygen_init(ctx) == 1);
   assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 3072) == 1);
   assert(EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

/* Serialize `key` as the PKCS#8 DER the vault hands the protected core. */
static size_t pkcs8_der(EVP_PKEY *key, unsigned char *out, size_t cap)
{
   PKCS8_PRIV_KEY_INFO *p8 = EVP_PKEY2PKCS8(key);
   assert(p8);
   int len = i2d_PKCS8_PRIV_KEY_INFO(p8, NULL);
   assert(len > 0 && (size_t)len < cap);
   unsigned char *cursor = out;
   assert(i2d_PKCS8_PRIV_KEY_INFO(p8, &cursor) == len);
   PKCS8_PRIV_KEY_INFO_free(p8);
   return (size_t)len;
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

/* Build a one-key JWKS for `key` under `kid` — the shape the server's JWKS
 * cache holds after the kb publication. */
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

static void record_init(kb_identity_token_authority_record_t *r, EVP_PKEY *key)
{
   memset(r, 0, sizeof(*r));
   r->newly_admitted = 1;
   memset(r->correlation_id, '1', 64);
   strcpy(r->jti, JTI);
   r->team_id = 7;
   strcpy(r->subject, SUBJECT);
   r->tier = KB_IDENTITY_TIER_DATA;
   strcpy(r->token_issuer, ISSUER);
   strcpy(r->audience, AUDIENCE);
   r->issued_at = 1000;
   r->expires_at = 1300;
   memset(r->installation_id, '4', 32);
   r->installation_generation = 1;
   r->installation_enrollment_id = 11;
   r->target_enrollment_id = 12;
   r->revocation_generation = 1;
   r->publication_generation = 1;
   memset(r->publication_candidate_id, '7', 64);
   memset(r->publication_manifest_sha256, 0x81, 32);
   memset(r->publication_envelope_sha256, 0x82, 32);
   strcpy(r->token_custody_key_id, "kms-token-root");
   r->token_version = 2;
   BIGNUM *modulus = NULL;
   assert(EVP_PKEY_get_bn_param(key, "n", &modulus) == 1);
   assert(BN_bn2binpad(modulus, r->token_public_key, sizeof(r->token_public_key)) ==
          (int)sizeof(r->token_public_key));
   BN_free(modulus);
   r->token_public_exponent[0] = 1;
   r->token_public_exponent[2] = 1;
   assert(kb_mgmt_token_kid(r->token_public_key, sizeof(r->token_public_key), r->kid,
                            sizeof(r->kid)) == 0);
   sha256(r->token_public_key, sizeof(r->token_public_key), r->token_public_digest);
   char jwk[KB_MGMT_TOKEN_JWK_MAX];
   size_t jwk_len = 0;
   assert(kb_mgmt_token_jwk(r->token_public_key, sizeof(r->token_public_key), jwk, sizeof(jwk),
                            &jwk_len) == 0);
   sha256(jwk, jwk_len, r->token_jwk_digest);
   r->vault_seal_epoch = 9;
   memcpy(r->hwm_attestation, "attestation", 11);
   r->hwm_attestation_len = 11;
   sha256(r->hwm_attestation, r->hwm_attestation_len, r->hwm_attestation_digest);
   r->envelope.seal_epoch = r->vault_seal_epoch;
   r->envelope.version = 2;
   r->envelope.ciphertext_len = 32;
   memset(r->envelope.ciphertext, 0x33, r->envelope.ciphertext_len);
   r->key_use_created_at_epoch = 999;
}

/* Mint from `record` and assert the result; on failure the output must be
 * cleared, never left holding a partial token. */
static void expect_result(const kb_identity_token_authority_record_t *record,
                          const unsigned char *der, size_t der_len,
                          kb_mgmt_token_authority_result_t want)
{
   char jwt[KB_IDENTITY_TOKEN_WIRE_MAX + 1];
   size_t jwt_len = 1;
   assert(kb_identity_token_authority_sign_pkcs8(record, der, der_len, jwt, sizeof(jwt),
                                                 &jwt_len) == want);
   if (want != KB_MGMT_TOKEN_AUTHORITY_OK)
      assert(jwt_len == 0 && jwt[0] == '\0');
}

/* Overwrite the record's subject with `prefix` followed by `hex_digits` 'a's,
 * zeroing the unused tail — the record's fields are canonical fixed strings, so
 * leaving stale bytes behind would fail validation for the wrong reason.
 * Returns whether the resulting record validates. */
static int set_subject(kb_identity_token_authority_record_t *r, const char *prefix,
                       size_t hex_digits)
{
   size_t n = strlen(prefix);
   assert(n + hex_digits < sizeof(r->subject));
   memset(r->subject, 0, sizeof(r->subject));
   memcpy(r->subject, prefix, n);
   memset(r->subject + n, 'a', hex_digits);
   return kb_identity_token_authority_record_valid(r);
}

int main(void)
{
   test_injected_record_validator();

   EVP_PKEY *key = rsa3072();
   unsigned char der[KB_MGMT_ROOT_SECRET_MAX + 1];
   size_t der_len = pkcs8_der(key, der, sizeof(der));

   kb_identity_token_authority_record_t record;
   record_init(&record, key);
   assert(kb_identity_token_authority_record_valid(&record));

   /* --- Round trip: the authority mints, the server's verifier accepts. --- */
   char jwt[KB_IDENTITY_TOKEN_WIRE_MAX + 1];
   size_t jwt_len = 0;
   assert(kb_identity_token_authority_sign_pkcs8(&record, der, der_len, jwt, sizeof(jwt),
                                                 &jwt_len) == KB_MGMT_TOKEN_AUTHORITY_OK);
   assert(jwt_len > 0 && jwt[jwt_len] == '\0' && strlen(jwt) == jwt_len);

   char jwks[8192];
   make_jwks(key, record.kid, jwks, sizeof(jwks));
   server_identity_token_claims_t claims;
   assert(server_identity_token_verify(jwt, jwt_len, jwks, ISSUER, AUDIENCE, record.issued_at,
                                       &claims) == SERVER_IDENTITY_TOKEN_OK);
   /* Every claim the server reads came from the admitted record, unaltered. */
   assert(strcmp(claims.issuer, ISSUER) == 0);
   assert(strcmp(claims.audience, AUDIENCE) == 0);
   assert(strcmp(claims.subject, SUBJECT) == 0);
   assert(strcmp(claims.jti, JTI) == 0);
   assert(strcmp(claims.kid, record.kid) == 0);
   assert(claims.team_id == record.team_id);
   assert(claims.tier == KB_IDENTITY_TIER_DATA);
   assert(claims.issued_at == record.issued_at && claims.expires_at == record.expires_at);

   /* The mint binds the token to exactly one audience: a server that is not the
    * record's audience must not accept it, even holding the same JWKS. */
   assert(server_identity_token_verify(jwt, jwt_len, jwks, ISSUER, "other-server", record.issued_at,
                                       &claims) == SERVER_IDENTITY_TOKEN_INVALID);

   /* --- Key admission is fail-closed. --- */
   /* Trailing bytes past the DER: the decode must consume the blob exactly. */
   der[der_len] = 0;
   expect_result(&record, der, der_len + 1, KB_MGMT_TOKEN_AUTHORITY_KEY_MISMATCH);

   EVP_PKEY *other = rsa3072();
   unsigned char other_der[KB_MGMT_ROOT_SECRET_MAX + 1];
   size_t other_len = pkcs8_der(other, other_der, sizeof(other_der));
   expect_result(&record, other_der, other_len, KB_MGMT_TOKEN_AUTHORITY_KEY_MISMATCH);

   /* A buffer that cannot hold the wire token is refused before signing. */
   char tiny[64];
   size_t tiny_len = 1;
   assert(kb_identity_token_authority_sign_pkcs8(&record, der, der_len, tiny, sizeof(tiny),
                                                 &tiny_len) ==
          KB_MGMT_TOKEN_AUTHORITY_OUTPUT_TOO_SMALL);
   assert(tiny_len == 0 && tiny[0] == '\0');

   /* --- Record admission is fail-closed: each violation alone is enough. --- */
   kb_identity_token_authority_record_t bad;

   bad = record; /* tier outside the three defined levels */
   bad.tier = (kb_identity_tier_t)3;
   assert(!kb_identity_token_authority_record_valid(&bad));
   expect_result(&bad, der, der_len, KB_MGMT_TOKEN_AUTHORITY_INVALID);

   bad = record; /* lifetime past the server's ceiling would be unusable */
   bad.expires_at = bad.issued_at + KB_IDENTITY_TOKEN_AUTHORITY_MAX_LIFETIME + 1;
   assert(!kb_identity_token_authority_record_valid(&bad));
   bad.expires_at = bad.issued_at + KB_IDENTITY_TOKEN_AUTHORITY_MAX_LIFETIME;
   assert(kb_identity_token_authority_record_valid(&bad));

   bad = record; /* non-increasing window */
   bad.expires_at = bad.issued_at;
   assert(!kb_identity_token_authority_record_valid(&bad));

   bad = record; /* a subject outside every accepted form */
   /* NOT "user-42": that is a legal bare host-account name, and the bare form is
    * the PAM login's subject (see tests/subject_corpus.h). An unprefixed string is
    * no longer invalid by default, so the fixture needs one that matches nothing —
    * a space, a leading dash, or a truncated prefix. */
   assert(!set_subject(&bad, "not a name", 0));
   assert(!set_subject(&bad, "-leading-dash", 0));
   assert(!set_subject(&bad, "oidc:idp:sub:extra", 0));
   assert(set_subject(&bad, "owner", 0));

   /* The bare PAM form IS accepted — the authority must not reject a subject the
    * database already admitted, which it did until the mint refused with
    * INTEGRITY after every gate had passed. */
   assert(set_subject(&bad, "alice", 0));
   assert(set_subject(&bad, "svc_user-1.2", 0));

   /* A cert serial is bounded at the 79 hex digits the server accepts. */
   assert(!set_subject(&bad, "cert:issuer-ca:", 80));
   assert(set_subject(&bad, "cert:issuer-ca:", 79));

   bad = record; /* a jti shorter than the verifier's 8-character floor */
   memset(bad.jti, 0, sizeof(bad.jti));
   strcpy(bad.jti, "short");
   assert(!kb_identity_token_authority_record_valid(&bad));
   memset(bad.jti, 0, sizeof(bad.jti));
   strcpy(bad.jti, "short-id");
   assert(kb_identity_token_authority_record_valid(&bad));

   bad = record; /* the published bindings must agree with the modulus */
   bad.token_jwk_digest[0] ^= 0xff;
   assert(!kb_identity_token_authority_record_valid(&bad));

   bad = record;
   bad.team_id = 0;
   assert(!kb_identity_token_authority_record_valid(&bad));

   bad = record;
   bad.token_public_exponent[2] = 3;
   assert(!kb_identity_token_authority_record_valid(&bad));

   bad = record;
   bad.envelope.seal_epoch = bad.vault_seal_epoch + 1;
   assert(!kb_identity_token_authority_record_valid(&bad));

   assert(!kb_identity_token_authority_record_valid(NULL));

   OPENSSL_cleanse(&record, sizeof(record));
   OPENSSL_cleanse(&bad, sizeof(bad));
   OPENSSL_cleanse(der, sizeof(der));
   OPENSSL_cleanse(other_der, sizeof(other_der));
   EVP_PKEY_free(other);
   EVP_PKEY_free(key);
   return 0;
}
