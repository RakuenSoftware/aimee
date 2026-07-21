#include "kb_mgmt_status.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void keys(unsigned char sk[32], unsigned char pk[32])
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key) == 1);
   size_t n = 32;
   assert(EVP_PKEY_get_raw_private_key(key, sk, &n) == 1 && n == 32);
   n = 32;
   assert(EVP_PKEY_get_raw_public_key(key, pk, &n) == 1 && n == 32);
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(ctx);
}

static kb_mgmt_status_t sample(void)
{
   kb_mgmt_status_t s = {0};
   s.version = 1;
   snprintf(s.key_id, sizeof(s.key_id), "status-key-1");
   for (size_t i = 0; i < sizeof(s.nonce); i++)
      s.nonce[i] = (unsigned char)i;
   snprintf(s.caller_issuer, sizeof(s.caller_issuer), "/CN=aimee-test-ca");
   snprintf(s.caller_serial_norm, sizeof(s.caller_serial_norm), "01ab");
   memset(s.caller_fingerprint, 'a', 64);
   s.caller_fingerprint[64] = '\0';
   snprintf(s.target_server_id, sizeof(s.target_server_id), "server-1");
   memset(s.target_mgmt_fingerprint, 'b', 64);
   s.target_mgmt_fingerprint[64] = '\0';
   snprintf(s.purpose, sizeof(s.purpose), "management.health.v1");
   s.issued_at = 1000;
   s.expires_at = 1010;
   s.revocation_generation = 7;
   return s;
}

int main(void)
{
   unsigned char sk[32], pk[32], wrong_sk[32], wrong_pk[32];
   keys(sk, pk);
   keys(wrong_sk, wrong_pk);
   kb_mgmt_status_t s = sample();
   assert(kb_mgmt_status_sign(&s, sk) == 0);
   assert(kb_mgmt_status_verify_signature(&s, pk) == 0);
   assert(kb_mgmt_status_verify_signature(&s, wrong_pk) == -1);
   assert(kb_mgmt_status_validate(&s, 1005, 7) == 0);
   assert(kb_mgmt_status_validate(&s, 1005, 8) == -1);
   assert(kb_mgmt_status_validate(&s, 1013, 7) == -1);

   char json[KB_MGMT_STATUS_JSON_MAX + 1];
   assert(kb_mgmt_status_to_json(&s, json, sizeof(json)) == 0);
   kb_mgmt_status_t parsed;
   assert(kb_mgmt_status_from_json(json, &parsed) == 0);
   assert(memcmp(&s, &parsed, sizeof(s)) == 0);
   assert(kb_mgmt_status_verify_signature(&parsed, pk) == 0);

   parsed.revocation_generation++;
   assert(kb_mgmt_status_verify_signature(&parsed, pk) == -1);
   parsed = s;
   parsed.nonce[0] ^= 1;
   assert(kb_mgmt_status_verify_signature(&parsed, pk) == -1);
   parsed = s;
   parsed.target_mgmt_fingerprint[0] = 'c';
   assert(kb_mgmt_status_verify_signature(&parsed, pk) == -1);

   const char *last = strrchr(json, '}');
   assert(last);
   char bad[KB_MGMT_STATUS_JSON_MAX + 1];
   size_t prefix = (size_t)(last - json);
   assert(prefix + 20 < sizeof(bad));
   memcpy(bad, json, prefix);
   snprintf(bad + prefix, sizeof(bad) - prefix, ",\"extra\":\"x\"}");
   assert(kb_mgmt_status_from_json(bad, &parsed) == -1);
   unsigned char recovered_nonce[KB_MGMT_STATUS_NONCE_LEN];
   assert(kb_mgmt_status_nonce_from_json(bad, recovered_nonce) == 0);
   assert(memcmp(recovered_nonce, s.nonce, sizeof(recovered_nonce)) == 0);

   const char *kid = strstr(json, "\"key_id\":\"status-key-1\"");
   assert(kid);
   prefix = (size_t)(kid - json);
   const char *suffix = kid + strlen("\"key_id\":\"status-key-1\"");
   assert(prefix + strlen(suffix) + 64 < sizeof(bad));
   snprintf(bad, sizeof(bad), "%.*s\"key_id\":\"status-key-1\",\"key_id\":\"status-key-1\"%s",
            (int)prefix, json, suffix);
   assert(kb_mgmt_status_from_json(bad, &parsed) == -1);
   assert(kb_mgmt_status_nonce_from_json(bad, recovered_nonce) == 0);

   const char *nonce_field = strstr(json, "\"nonce\":");
   assert(nonce_field);
   prefix = (size_t)(nonce_field - json);
   snprintf(bad, sizeof(bad), "%.*s\"nonce\":\"AA\",%s", (int)prefix, json, nonce_field);
   assert(kb_mgmt_status_from_json(bad, &parsed) == -1);
   assert(kb_mgmt_status_nonce_from_json(bad, recovered_nonce) == -1);

   snprintf(bad, sizeof(bad), "%s ", json);
   assert(kb_mgmt_status_from_json(bad, &parsed) == -1);
   puts("kb_mgmt_status: ok");
   return 0;
}
