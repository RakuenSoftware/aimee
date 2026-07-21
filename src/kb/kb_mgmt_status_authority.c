#include "kb_mgmt_status_authority.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

int kb_mgmt_status_authority_issue(const kb_mgmt_status_request_t *r, const char *issuer,
                                   const char *serial, const char *fingerprint, const char *key_id,
                                   uint64_t now, kb_mgmt_status_lookup_fn lookup, void *lookup_ctx,
                                   kb_mgmt_status_sign_fn sign, void *sign_ctx,
                                   kb_mgmt_status_t *out)
{
   if (!r || !issuer || !serial || !fingerprint || !key_id || !lookup || !sign || !out ||
       !r->target_server_id[0] || strlen(r->target_server_id) > 127 ||
       strlen(r->target_mgmt_fingerprint) != 64 ||
       strcmp(r->purpose, "management.health.v1") != 0 || !issuer[0] || strlen(issuer) > 600 ||
       !serial[0] || strlen(serial) > 128 || strlen(fingerprint) != 64 || !key_id[0] ||
       strlen(key_id) > 64 || now == 0 || now > UINT64_MAX - 10)
      return -1;
   int64_t generation = 0;
   char authoritative_fp[65] = "";
   if (lookup(issuer, serial, fingerprint, r->target_server_id, r->purpose, &generation,
              authoritative_fp, sizeof(authoritative_fp), lookup_ctx) != 0 ||
       generation < 1 || strlen(authoritative_fp) != 64 ||
       CRYPTO_memcmp(authoritative_fp, r->target_mgmt_fingerprint, 64) != 0)
      return -1;
   memset(out, 0, sizeof(*out));
   out->version = 1;
   memcpy(out->nonce, r->nonce, sizeof(out->nonce));
   snprintf(out->key_id, sizeof(out->key_id), "%s", key_id);
   snprintf(out->caller_issuer, sizeof(out->caller_issuer), "%s", issuer);
   snprintf(out->caller_serial_norm, sizeof(out->caller_serial_norm), "%s", serial);
   snprintf(out->caller_fingerprint, sizeof(out->caller_fingerprint), "%s", fingerprint);
   snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", r->target_server_id);
   snprintf(out->target_mgmt_fingerprint, sizeof(out->target_mgmt_fingerprint), "%s",
            authoritative_fp);
   snprintf(out->purpose, sizeof(out->purpose), "%s", r->purpose);
   out->issued_at = now;
   out->expires_at = now + 10;
   out->revocation_generation = (uint64_t)generation;
   if (sign(out, sign_ctx) != 0)
   {
      OPENSSL_cleanse(out, sizeof(*out));
      return -1;
   }
   return 0;
}
