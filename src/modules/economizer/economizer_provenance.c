#include "economizer_provenance.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct econ_provenance
{
   econ_provenance_binding_t binding;
   unsigned char source_digest[32];
   size_t source_len;
   atomic_int consumed;
};

static int binding_valid(const econ_provenance_binding_t *b)
{
   return b && b->tenant_id && b->task_id && b->call_id && b->semantic_contract_id &&
          b->transform_id && b->transform_version;
}

static int binding_equal(const econ_provenance_binding_t *a, const econ_provenance_binding_t *b)
{
   return a->tenant_id == b->tenant_id && a->task_id == b->task_id && a->call_id == b->call_id &&
          a->semantic_contract_id == b->semantic_contract_id &&
          a->transform_id == b->transform_id && a->transform_version == b->transform_version;
}

static int digest_bytes(const void *source, size_t source_len, unsigned char out[32])
{
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(ctx, source, source_len) == 1 &&
            EVP_DigestFinal_ex(ctx, out, &n) == 1 && n == 32;
   EVP_MD_CTX_free(ctx);
   return ok ? 0 : -1;
}

int econ_provenance_issue_local(const econ_provenance_binding_t *binding, const void *source,
                                size_t source_len, econ_provenance_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!binding_valid(binding) || (!source && source_len) ||
       source_len > ECON_PROVENANCE_MAX_SOURCE)
      return -1;
   econ_provenance_t *cap = calloc(1, sizeof(*cap));
   if (!cap)
      return -1;
   cap->binding = *binding;
   cap->source_len = source_len;
   atomic_init(&cap->consumed, 0);
   if (digest_bytes(source, source_len, cap->source_digest) != 0)
   {
      free(cap);
      return -1;
   }
   *out = cap;
   return 0;
}

int econ_provenance_consume(econ_provenance_t *cap, const econ_provenance_binding_t *expected,
                            const void *source, size_t source_len)
{
   if (!cap || !binding_valid(expected) || (!source && source_len) ||
       source_len > ECON_PROVENANCE_MAX_SOURCE || !binding_equal(&cap->binding, expected) ||
       cap->source_len != source_len)
      return -1;
   unsigned char digest[32];
   if (digest_bytes(source, source_len, digest) != 0 ||
       CRYPTO_memcmp(cap->source_digest, digest, sizeof(digest)) != 0)
   {
      OPENSSL_cleanse(digest, sizeof(digest));
      return -1;
   }
   OPENSSL_cleanse(digest, sizeof(digest));
   int expected_state = 0;
   return atomic_compare_exchange_strong_explicit(&cap->consumed, &expected_state, 1,
                                                  memory_order_acq_rel, memory_order_acquire)
              ? 0
              : -1;
}

void econ_provenance_destroy(econ_provenance_t *cap)
{
   if (!cap)
      return;
   OPENSSL_cleanse(cap, sizeof(*cap));
   free(cap);
}
