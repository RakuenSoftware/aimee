#include "kb_mgmt_status_authority.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static int lookup(const char *issuer, const char *serial, const char *fp, const char *target,
                  const char *purpose, int64_t *generation, char *target_fp, size_t cap, void *ctx)
{
   (void)ctx;
   if (strcmp(issuer, "/CN=ca") || strcmp(serial, "01") || strlen(fp) != 64 ||
       strcmp(target, "server-1") || strcmp(purpose, "management.health.v1"))
      return -1;
   *generation = 9;
   snprintf(target_fp, cap, "%064d", 2);
   return 0;
}

static int sign_status(kb_mgmt_status_t *s, void *ctx)
{
   return kb_mgmt_status_sign(s, ctx);
}

int main(void)
{
   kb_mgmt_status_request_t r = {0};
   snprintf(r.target_server_id, sizeof(r.target_server_id), "server-1");
   snprintf(r.target_mgmt_fingerprint, sizeof(r.target_mgmt_fingerprint), "%064d", 2);
   snprintf(r.purpose, sizeof(r.purpose), "management.health.v1");
   memset(r.nonce, 7, sizeof(r.nonce));
   char caller_fp[65];
   memset(caller_fp, 'a', 64);
   caller_fp[64] = '\0';
   unsigned char sk[32], pk[32];
   EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   EVP_PKEY *key = NULL;
   assert(kctx && EVP_PKEY_keygen_init(kctx) == 1 && EVP_PKEY_keygen(kctx, &key) == 1);
   size_t n = 32;
   assert(EVP_PKEY_get_raw_private_key(key, sk, &n) == 1);
   n = 32;
   assert(EVP_PKEY_get_raw_public_key(key, pk, &n) == 1);
   kb_mgmt_status_t out;
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, lookup, NULL,
                                         sign_status, sk, &out) == 0);
   assert(out.revocation_generation == 9 && out.expires_at == 110);
   assert(kb_mgmt_status_verify_signature(&out, pk) == 0);
   r.target_mgmt_fingerprint[0] = 'f';
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, lookup, NULL,
                                         sign_status, sk, &out) == -1);
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(kctx);
   puts("kb_mgmt_status_authority: ok");
   return 0;
}
