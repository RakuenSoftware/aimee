#define _POSIX_C_SOURCE 200809L
#include "kb_management_cert_binding.h"
#include "kb_management_cert_crypto.h"

#include <openssl/crypto.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
   uint8_t *p;
   size_t left;
} binding_writer_t;

static const uint8_t attest_domain[] = "aimee.p5.management-attest.v1";
static const uint8_t intent_domain[] = "aimee.p5.management-key-intent-custody.v1";
static const uint8_t candidate_domain[] = "aimee.p5.management-bundle-custody.v1";

static int put(binding_writer_t *w, const void *p, size_t n)
{
   if (!w || (!p && n) || n > w->left)
      return -1;
   if (n)
      memcpy(w->p, p, n);
   w->p += n;
   w->left -= n;
   return 0;
}

static int put_u32(binding_writer_t *w, uint32_t v)
{
   uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
   return put(w, b, sizeof(b));
}

static int put_u64(binding_writer_t *w, uint64_t v)
{
   uint8_t b[8];
   for (size_t i = 0; i < sizeof(b); ++i)
      b[sizeof(b) - i - 1] = (uint8_t)(v >> (i * 8));
   return put(w, b, sizeof(b));
}

static int put_field(binding_writer_t *w, const void *p, size_t n)
{
   return n > UINT32_MAX || put_u32(w, (uint32_t)n) || put(w, p, n) ? -1 : 0;
}

static int exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int text_len(const char *s, size_t max, size_t *out)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return -1;
   for (size_t i = 0; i < n; ++i)
      if ((uint8_t)s[i] < 0x21 || (uint8_t)s[i] > 0x7e)
         return -1;
   *out = n;
   return 0;
}

static int valid_kind(kb_workload_provider_kind_t kind)
{
   return kind >= KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 && kind <= KB_WORKLOAD_PROVIDER_PKCS11_V1;
}

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
   if (!a || !b || !an || !bn)
      return 0;
   uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
   return ap < bp ? bp - ap < an : ap - bp < bn;
}

static int begin_output(uint8_t *out, size_t cap, size_t *out_len, binding_writer_t *w)
{
   if (!out || !out_len || !w || cap > KB_MANAGEMENT_CERT_TRANSCRIPT_MAX)
      return -1;
   *out_len = 0;
   if (cap)
      OPENSSL_cleanse(out, cap);
   w->p = out;
   w->left = cap;
   return 0;
}

static int finish_output(binding_writer_t *w, size_t cap, size_t *out_len)
{
   *out_len = cap - w->left;
   return 0;
}

static int fail_output(uint8_t *out, size_t cap, size_t *out_len)
{
   if (out && cap <= KB_MANAGEMENT_CERT_TRANSCRIPT_MAX)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   return -1;
}

int kb_management_cert_attest_transcript(const char installation_id[33],
                                         kb_workload_provider_kind_t kind, uint8_t *out, size_t cap,
                                         size_t *out_len)
{
   binding_writer_t w;
   if (overlap(out, cap, installation_id, 33) || overlap(out, cap, out_len, sizeof(*out_len)))
      return -1;
   if (overlap(out_len, sizeof(*out_len), installation_id, 33))
      return -1;
   if (begin_output(out, cap, out_len, &w))
      return -1;
   if (!exact_hex(installation_id, 32) || !valid_kind(kind) ||
       put(&w, attest_domain, sizeof(attest_domain) - 1) || put_field(&w, installation_id, 32) ||
       put_u32(&w, (uint32_t)kind))
      return fail_output(out, cap, out_len);
   return finish_output(&w, cap, out_len);
}

static int intent_fields(const kb_management_cert_intent_binding_t *v, binding_writer_t *w,
                         int with_domain)
{
   size_t issuer_len = 0, subject_len = 0;
   if (!v || !exact_hex(v->installation_id, 32) || !exact_hex(v->lineage_id, 32) ||
       !exact_hex(v->operation_id, 64) || !exact_hex(v->authority_id, 32) ||
       !exact_hex(v->storage_id, 32) || v->generation < 1 || !valid_kind(v->provider_kind) ||
       text_len(v->workload_issuer, 600, &issuer_len) ||
       text_len(v->workload_subject, 600, &subject_len))
      return -1;
   return (with_domain && put(w, intent_domain, sizeof(intent_domain) - 1)) ||
                  put_field(w, v->installation_id, 32) || put_field(w, v->lineage_id, 32) ||
                  put_u64(w, (uint64_t)v->generation) || put_field(w, v->operation_id, 64) ||
                  put_field(w, v->authority_id, 32) || put(w, v->binding_digest, 32) ||
                  put_field(w, v->workload_issuer, issuer_len) ||
                  put_field(w, v->workload_subject, subject_len) || put(w, v->proof_anchor, 32) ||
                  put(w, v->custody_anchor, 32) || put(w, v->csr_digest, 32) ||
                  put(w, v->csr_spki_digest, 32) || put_u32(w, (uint32_t)v->provider_kind) ||
                  put(w, v->nonce, 32) || put_field(w, v->storage_id, 32)
              ? -1
              : 0;
}

int kb_management_cert_intent_transcript(const kb_management_cert_intent_binding_t *v, uint8_t *out,
                                         size_t cap, size_t *out_len)
{
   binding_writer_t w;
   if (overlap(out, cap, v, sizeof(*v)) || overlap(out, cap, out_len, sizeof(*out_len)) ||
       overlap(out_len, sizeof(*out_len), v, sizeof(*v)))
      return -1;
   if (begin_output(out, cap, out_len, &w))
      return -1;
   if (intent_fields(v, &w, 1))
      return fail_output(out, cap, out_len);
   return finish_output(&w, cap, out_len);
}

int kb_management_cert_candidate_transcript(const kb_management_cert_candidate_binding_t *v,
                                            uint8_t *out, size_t cap, size_t *out_len)
{
   binding_writer_t w;
   size_t ca_issuer_len = 0, leaf_issuer_len = 0, serial_len = 0;
   if (overlap(out, cap, v, sizeof(*v)) || overlap(out, cap, out_len, sizeof(*out_len)) ||
       overlap(out_len, sizeof(*out_len), v, sizeof(*v)))
      return -1;
   if (begin_output(out, cap, out_len, &w))
      return -1;
   if (!v || v->not_before_epoch < 1 || v->not_after_epoch <= v->not_before_epoch ||
       text_len(v->ca_issuer, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX, &ca_issuer_len) ||
       text_len(v->leaf_issuer, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX, &leaf_issuer_len) ||
       text_len(v->leaf_serial_norm, DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX, &serial_len) ||
       !exact_hex(v->leaf_serial_norm, serial_len) ||
       put(&w, candidate_domain, sizeof(candidate_domain) - 1) ||
       intent_fields(&v->intent, &w, 0) || put_field(&w, v->ca_issuer, ca_issuer_len) ||
       put(&w, v->ca_fingerprint, 32) || put_field(&w, v->leaf_issuer, leaf_issuer_len) ||
       put_field(&w, v->leaf_serial_norm, serial_len) || put(&w, v->leaf_fingerprint, 32) ||
       put(&w, v->leaf_spki_digest, 32) || put_u64(&w, (uint64_t)v->not_before_epoch) ||
       put_u64(&w, (uint64_t)v->not_after_epoch) || put(&w, v->public_bundle_digest, 32))
      return fail_output(out, cap, out_len);
   return finish_output(&w, cap, out_len);
}

static int hash_transcript(int (*build)(const void *, uint8_t *, size_t, size_t *),
                           const void *value, uint8_t out[32])
{
   uint8_t transcript[KB_MANAGEMENT_CERT_TRANSCRIPT_MAX];
   size_t len = 0;
   if (!out)
      return -1;
   memset(out, 0, 32);
   int rc = build(value, transcript, sizeof(transcript), &len) ||
                    kb_management_cert_sha256(transcript, len, out)
                ? -1
                : 0;
   OPENSSL_cleanse(transcript, sizeof(transcript));
   return rc;
}

static int intent_adapter(const void *v, uint8_t *out, size_t cap, size_t *len)
{
   return kb_management_cert_intent_transcript(v, out, cap, len);
}

static int candidate_adapter(const void *v, uint8_t *out, size_t cap, size_t *len)
{
   return kb_management_cert_candidate_transcript(v, out, cap, len);
}

int kb_management_cert_attest_binding(const char installation_id[33],
                                      kb_workload_provider_kind_t kind, uint8_t out[32])
{
   uint8_t transcript[128];
   size_t len = 0;
   if (!out || overlap(out, 32, installation_id, 33))
      return -1;
   memset(out, 0, 32);
   int rc = kb_management_cert_attest_transcript(installation_id, kind, transcript,
                                                 sizeof(transcript), &len) ||
                    kb_management_cert_sha256(transcript, len, out)
                ? -1
                : 0;
   OPENSSL_cleanse(transcript, sizeof(transcript));
   return rc;
}

int kb_management_cert_intent_binding(const kb_management_cert_intent_binding_t *v, uint8_t out[32])
{
   if (!out || overlap(out, 32, v, sizeof(*v)))
      return -1;
   return hash_transcript(intent_adapter, v, out);
}

int kb_management_cert_candidate_binding(const kb_management_cert_candidate_binding_t *v,
                                         uint8_t out[32])
{
   if (!out || overlap(out, 32, v, sizeof(*v)))
      return -1;
   return hash_transcript(candidate_adapter, v, out);
}
