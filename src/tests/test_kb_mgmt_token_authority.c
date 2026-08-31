#include "management_token_authority.h"

#include <assert.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <string.h>

static int contract_result;

static int management_record_valid_contract(const kb_mgmt_token_authority_record_t *record)
{
   assert(record);
   return contract_result;
}

static int identity_record_valid_unused(const kb_identity_token_authority_record_t *record)
{
   (void)record;
   return 1;
}

static void test_injected_record_validator(void)
{
   kb_mgmt_token_authority_record_t record = {0};

   aimee_db2_register_token_record_validators(NULL, identity_record_valid_unused);
   assert(!db2_management_token_authority_record_validate(&record));

   aimee_db2_register_token_record_validators(management_record_valid_contract,
                                              identity_record_valid_unused);
   contract_result = 1;
   assert(db2_management_token_authority_record_validate(&record));
   contract_result = 0;
   assert(!db2_management_token_authority_record_validate(&record));
   contract_result = 2;
   assert(!db2_management_token_authority_record_validate(&record));
   contract_result = -1;
   assert(!db2_management_token_authority_record_validate(&record));
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

static void record_init(kb_mgmt_token_authority_record_t *r, EVP_PKEY *key)
{
   memset(r, 0, sizeof(*r));
   r->newly_admitted = 1;
   memset(r->correlation_id, '1', 64);
   memset(r->jti, '2', 64);
   r->team_id = 7;
   strcpy(r->actor_identity, "owner");
   r->capability = KB_MGMT_TOKEN_CAP_REMOTE_WRITES;
   strcpy(r->target_server_id, "server-1");
   memset(r->request_sha256, '3', 64);
   strcpy(r->token_issuer, "https://kb.example.test");
   strcpy(r->audience, "server-1");
   r->issued_at = 1000;
   r->expires_at = 1090;
   memset(r->installation_id, '4', 32);
   r->installation_generation = 1;
   r->installation_enrollment_id = 11;
   strcpy(r->local_cert_issuer, "local-ca");
   strcpy(r->local_cert_serial_norm, "01");
   memset(r->local_cert_fingerprint, '5', 64);
   r->target_enrollment_id = 12;
   strcpy(r->target_mgmt_issuer, "target-ca");
   strcpy(r->target_mgmt_serial_norm, "02");
   memset(r->target_mgmt_fingerprint, '6', 64);
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

int main(void)
{
   test_injected_record_validator();

   EVP_PKEY *key = rsa3072();
   PKCS8_PRIV_KEY_INFO *p8 = EVP_PKEY2PKCS8(key);
   assert(p8);
   int der_len = i2d_PKCS8_PRIV_KEY_INFO(p8, NULL);
   assert(der_len > 0);
   unsigned char der[KB_MGMT_ROOT_SECRET_MAX + 1];
   unsigned char *cursor = der;
   assert(i2d_PKCS8_PRIV_KEY_INFO(p8, &cursor) == der_len);

   kb_mgmt_token_authority_record_t record;
   record_init(&record, key);
   assert(kb_mgmt_token_authority_record_valid(&record));
   char jwt[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t jwt_len = 0;
   assert(kb_mgmt_token_authority_sign_pkcs8(&record, der, (size_t)der_len, jwt, sizeof(jwt),
                                             &jwt_len) == KB_MGMT_TOKEN_AUTHORITY_OK);
   assert(jwt_len > 0 && jwt[jwt_len] == '\0');
   assert(strchr(jwt, '.') && strrchr(jwt, '.') != strchr(jwt, '.'));

   der[der_len] = 0;
   assert(kb_mgmt_token_authority_sign_pkcs8(&record, der, (size_t)der_len + 1, jwt, sizeof(jwt),
                                             &jwt_len) != KB_MGMT_TOKEN_AUTHORITY_OK);
   assert(jwt_len == 0 && jwt[0] == '\0');
   record.token_public_exponent[2] = 3;
   assert(!kb_mgmt_token_authority_record_valid(&record));

   OPENSSL_cleanse(&record, sizeof(record));
   OPENSSL_cleanse(der, sizeof(der));
   PKCS8_PRIV_KEY_INFO_free(p8);
   EVP_PKEY_free(key);
   return 0;
}
