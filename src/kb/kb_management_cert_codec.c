#define _POSIX_C_SOURCE 200809L
#include "kb_management_cert_codec.h"

#include <limits.h>
#include <string.h>

typedef struct
{
   uint8_t *p;
   size_t left;
} writer_t;

typedef struct
{
   const uint8_t *p;
   size_t left;
} reader_t;

static const uint8_t key_magic[] = "aimee.p5.management-key-intent.v1";
static const uint8_t bundle_magic[] = "aimee.p5.management-bundle.v1";
static const uint8_t intent_magic[] = "aimee.p5.management-intent-record.v1";
static const uint8_t candidate_magic[] = "aimee.p5.management-candidate.v1";
static const uint8_t manifest_magic[] = "aimee.p5.management-current.v1";
static const uint8_t pending_magic[] = "aimee.p5.management-pending.v1";

static int put(writer_t *w, const void *p, size_t n)
{
   if (!w || (!p && n) || n > w->left)
      return -1;
   if (n)
      memcpy(w->p, p, n);
   w->p += n;
   w->left -= n;
   return 0;
}

static int put_u16(writer_t *w, uint16_t value)
{
   uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)value};
   return put(w, b, sizeof(b));
}

static int put_u32(writer_t *w, uint32_t value)
{
   uint8_t b[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                   (uint8_t)value};
   return put(w, b, sizeof(b));
}

static int put_u64(writer_t *w, uint64_t value)
{
   uint8_t b[8];
   for (size_t i = 0; i < sizeof(b); ++i)
      b[sizeof(b) - 1 - i] = (uint8_t)(value >> (i * 8));
   return put(w, b, sizeof(b));
}

static int get(reader_t *r, const uint8_t **p, size_t n)
{
   if (!r || !p || n > r->left)
      return -1;
   *p = r->p;
   r->p += n;
   r->left -= n;
   return 0;
}

static int get_u16(reader_t *r, uint16_t *value)
{
   const uint8_t *p;
   if (!value || get(r, &p, 2))
      return -1;
   *value = (uint16_t)((uint16_t)p[0] << 8 | p[1]);
   return 0;
}

static int get_u32(reader_t *r, uint32_t *value)
{
   const uint8_t *p;
   if (!value || get(r, &p, 4))
      return -1;
   *value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
   return 0;
}

static int get_u64(reader_t *r, uint64_t *value)
{
   const uint8_t *p;
   if (!value || get(r, &p, 8))
      return -1;
   uint64_t v = 0;
   for (size_t i = 0; i < 8; ++i)
      v = (v << 8) | p[i];
   *value = v;
   return 0;
}

static int exact_magic(reader_t *r, const uint8_t *magic, size_t n)
{
   const uint8_t *p;
   return get(r, &p, n) || memcmp(p, magic, n) ? -1 : 0;
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

static int copy_fixed(reader_t *, char *, size_t);

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
   if (!a || !b || !an || !bn)
      return 0;
   uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
   return ap < bp ? bp - ap < an : ap - bp < bn;
}

static int add_size(size_t *total, size_t value)
{
   if (!total || value > SIZE_MAX - *total)
      return -1;
   *total += value;
   return 0;
}

static int printable(const char *s, size_t max, size_t *len)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((uint8_t)s[i] < 0x21 || (uint8_t)s[i] > 0x7e)
         return 0;
   *len = n;
   return 1;
}

static int encode_fields(const uint8_t *magic, size_t magic_len, const void *a, size_t a_len,
                         const void *b, size_t b_len, const void *c, size_t c_len, uint8_t *out,
                         size_t cap, size_t *out_len)
{
   if (!out || !out_len || overlap(out, cap, a, a_len) || overlap(out, cap, b, b_len) ||
       (c && overlap(out, cap, c, c_len)) || overlap(out, cap, out_len, sizeof(*out_len)))
      return -1;
   if (overlap(out_len, sizeof(*out_len), a, a_len) ||
       overlap(out_len, sizeof(*out_len), b, b_len) ||
       (c && overlap(out_len, sizeof(*out_len), c, c_len)))
      return -1;
   *out_len = 0;
   if (cap)
      memset(out, 0, cap);
   if (!a || !b || (c_len && !c) || !a_len || !b_len || a_len > UINT32_MAX || b_len > UINT32_MAX ||
       c_len > UINT32_MAX)
      return -1;
   size_t total = magic_len;
   if (add_size(&total, 4) || add_size(&total, a_len) || add_size(&total, 4) ||
       add_size(&total, b_len) || (c && (add_size(&total, 4) || add_size(&total, c_len))))
      return -1;
   if (total > cap || total > KB_MANAGEMENT_CERT_PLAINTEXT_MAX)
      return -1;
   writer_t w = {out, cap};
   if (put(&w, magic, magic_len) || put_u32(&w, (uint32_t)a_len) || put(&w, a, a_len) ||
       put_u32(&w, (uint32_t)b_len) || put(&w, b, b_len) ||
       (c && (put_u32(&w, (uint32_t)c_len) || put(&w, c, c_len))))
   {
      return -1;
   }
   *out_len = total;
   return 0;
}

int kb_management_cert_key_intent_encode(const void *key, size_t key_len, const void *csr,
                                         size_t csr_len, uint8_t *out, size_t cap, size_t *out_len)
{
   return encode_fields(key_magic, sizeof(key_magic) - 1, key, key_len, csr, csr_len, NULL, 0, out,
                        cap, out_len);
}

static int decode_fields(const uint8_t *magic, size_t magic_len, const void *input, size_t len,
                         const uint8_t **a, size_t *a_len, const uint8_t **b, size_t *b_len,
                         const uint8_t **c, size_t *c_len)
{
   if (!input || !len || len > KB_MANAGEMENT_CERT_PLAINTEXT_MAX || !a || !a_len || !b || !b_len ||
       (!!c != !!c_len))
      return -1;
   reader_t r = {input, len};
   uint32_t n;
   if (exact_magic(&r, magic, magic_len) || get_u32(&r, &n) || !n || get(&r, a, n))
      return -1;
   *a_len = n;
   if (get_u32(&r, &n) || !n || get(&r, b, n))
      return -1;
   *b_len = n;
   if (c)
   {
      if (get_u32(&r, &n) || !n || get(&r, c, n))
         return -1;
      *c_len = n;
   }
   return r.left ? -1 : 0;
}

int kb_management_cert_key_intent_decode(const void *input, size_t len,
                                         kb_management_cert_key_intent_view_t *out)
{
   if (!out || overlap(input, len, out, sizeof(*out)))
      return -1;
   memset(out, 0, sizeof(*out));
   kb_management_cert_key_intent_view_t v = {0};
   if (decode_fields(key_magic, sizeof(key_magic) - 1, input, len, &v.key_der, &v.key_der_len,
                     &v.csr_der, &v.csr_der_len, NULL, NULL))
      return -1;
   *out = v;
   return 0;
}

int kb_management_cert_bundle_encode(const void *key, size_t key_len, const void *leaf,
                                     size_t leaf_len, const void *ca, size_t ca_len, uint8_t *out,
                                     size_t cap, size_t *out_len)
{
   return encode_fields(bundle_magic, sizeof(bundle_magic) - 1, key, key_len, leaf, leaf_len, ca,
                        ca_len, out, cap, out_len);
}

int kb_management_cert_bundle_decode(const void *input, size_t len,
                                     kb_management_cert_bundle_view_t *out)
{
   if (!out || overlap(input, len, out, sizeof(*out)))
      return -1;
   memset(out, 0, sizeof(*out));
   kb_management_cert_bundle_view_t v = {0};
   if (decode_fields(bundle_magic, sizeof(bundle_magic) - 1, input, len, &v.key_der, &v.key_der_len,
                     &v.leaf_der, &v.leaf_der_len, &v.ca_der, &v.ca_der_len))
      return -1;
   *out = v;
   return 0;
}

int kb_management_cert_intent_encode(const kb_management_cert_intent_view_t *v, uint8_t *out,
                                     size_t cap, size_t *out_len)
{
   if (!v || !out || !out_len || overlap(out, cap, v, sizeof(*v)) ||
       overlap(out, cap, v->ciphertext, v->ciphertext_len) ||
       overlap(out, cap, out_len, sizeof(*out_len)) ||
       overlap(out_len, sizeof(*out_len), v, sizeof(*v)) ||
       overlap(out_len, sizeof(*out_len), v->ciphertext, v->ciphertext_len))
      return -1;
   *out_len = 0;
   if (cap)
      memset(out, 0, cap);
   if (!exact_hex(v->installation_id, 32) || !exact_hex(v->lineage_id, 32) ||
       !exact_hex(v->operation_id, 64) || !exact_hex(v->authority_id, 32) ||
       !exact_hex(v->storage_id, 32) || v->generation < 1 ||
       v->provider_kind < KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 ||
       v->provider_kind > KB_WORKLOAD_PROVIDER_PKCS11_V1 || !v->ciphertext || !v->ciphertext_len ||
       v->ciphertext_len > KB_WORKLOAD_WRAP_CAP)
      return -1;
   size_t total = sizeof(intent_magic) - 1 + 32 + 32 + 64 + 32 + 32 + 8 + 4 + 32 * 5 + 4;
   if (add_size(&total, v->ciphertext_len))
      return -1;
   if (total > cap || total > KB_MANAGEMENT_CERT_CANDIDATE_MAX)
      return -1;
   writer_t w = {out, cap};
   if (put(&w, intent_magic, sizeof(intent_magic) - 1) || put(&w, v->installation_id, 32) ||
       put(&w, v->lineage_id, 32) || put(&w, v->operation_id, 64) || put(&w, v->authority_id, 32) ||
       put(&w, v->storage_id, 32) || put_u64(&w, (uint64_t)v->generation) ||
       put_u32(&w, (uint32_t)v->provider_kind) || put(&w, v->nonce, 32) ||
       put(&w, v->binding_digest, 32) || put(&w, v->csr_digest, 32) ||
       put(&w, v->csr_spki_digest, 32) || put(&w, v->custody_binding_digest, 32) ||
       put_u32(&w, (uint32_t)v->ciphertext_len) || put(&w, v->ciphertext, v->ciphertext_len))
   {
      return -1;
   }
   *out_len = total;
   return 0;
}

int kb_management_cert_intent_decode(const void *input, size_t len,
                                     kb_management_cert_intent_view_t *out)
{
   if (!out || overlap(input, len, out, sizeof(*out)))
      return -1;
   memset(out, 0, sizeof(*out));
   if (!input || len > KB_MANAGEMENT_CERT_CANDIDATE_MAX)
      return -1;
   reader_t r = {input, len};
   kb_management_cert_intent_view_t v = {0};
   uint64_t generation;
   uint32_t kind, cipher_len;
   const uint8_t *p;
   if (exact_magic(&r, intent_magic, sizeof(intent_magic) - 1) ||
       copy_fixed(&r, v.installation_id, 32) || copy_fixed(&r, v.lineage_id, 32) ||
       copy_fixed(&r, v.operation_id, 64) || copy_fixed(&r, v.authority_id, 32) ||
       copy_fixed(&r, v.storage_id, 32) || get_u64(&r, &generation) || generation < 1 ||
       generation > INT64_MAX || get_u32(&r, &kind) || kind < KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 ||
       kind > KB_WORKLOAD_PROVIDER_PKCS11_V1)
      return -1;
   v.generation = (int64_t)generation;
   v.provider_kind = (kb_workload_provider_kind_t)kind;
#define INTENT_TAKE32(field) (get(&r, &p, 32) || (memcpy((field), p, 32), 0))
   if (INTENT_TAKE32(v.nonce) || INTENT_TAKE32(v.binding_digest) || INTENT_TAKE32(v.csr_digest) ||
       INTENT_TAKE32(v.csr_spki_digest) || INTENT_TAKE32(v.custody_binding_digest) ||
       get_u32(&r, &cipher_len) || !cipher_len || cipher_len > KB_WORKLOAD_WRAP_CAP ||
       get(&r, &p, cipher_len) || r.left)
      return -1;
#undef INTENT_TAKE32
   v.ciphertext = p;
   v.ciphertext_len = cipher_len;
   *out = v;
   return 0;
}

int kb_management_cert_candidate_encode(const kb_management_cert_candidate_view_t *v, uint8_t *out,
                                        size_t cap, size_t *out_len)
{
   if (!v || !out || !out_len || overlap(out, cap, v, sizeof(*v)) ||
       overlap(out, cap, v->ciphertext, v->ciphertext_len) ||
       overlap(out, cap, out_len, sizeof(*out_len)) ||
       overlap(out_len, sizeof(*out_len), v, sizeof(*v)) ||
       overlap(out_len, sizeof(*out_len), v->ciphertext, v->ciphertext_len))
      return -1;
   *out_len = 0;
   if (cap)
      memset(out, 0, cap);
   size_t issuer_len = 0, ca_issuer_len = 0, serial_len = 0;
   if (!exact_hex(v->installation_id, 32) || !exact_hex(v->lineage_id, 32) ||
       !exact_hex(v->operation_id, 64) || !exact_hex(v->authority_id, 32) ||
       !exact_hex(v->storage_id, 32) || v->generation < 1 ||
       v->provider_kind < KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 ||
       v->provider_kind > KB_WORKLOAD_PROVIDER_PKCS11_V1 ||
       !printable(v->issuer, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX, &issuer_len) ||
       !printable(v->ca_issuer, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX, &ca_issuer_len) ||
       !printable(v->serial_norm, DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX, &serial_len) ||
       !exact_hex(v->serial_norm, serial_len) || v->not_before_epoch < 1 ||
       v->not_after_epoch <= v->not_before_epoch || !v->ciphertext || !v->ciphertext_len ||
       v->ciphertext_len > KB_WORKLOAD_WRAP_CAP || issuer_len > UINT16_MAX ||
       ca_issuer_len > UINT16_MAX || serial_len > UINT16_MAX)
      return -1;
   size_t total = sizeof(candidate_magic) - 1 + 32 + 32 + 64 + 32 + 32 + 8 + 4 + 32 * 9 + 2;
   if (add_size(&total, issuer_len) || add_size(&total, 2) || add_size(&total, ca_issuer_len) ||
       add_size(&total, 2) || add_size(&total, serial_len) || add_size(&total, 8 + 8 + 4) ||
       add_size(&total, v->ciphertext_len))
      return -1;
   if (total > cap || total > KB_MANAGEMENT_CERT_CANDIDATE_MAX)
      return -1;
   writer_t w = {out, cap};
   int bad =
       put(&w, candidate_magic, sizeof(candidate_magic) - 1) || put(&w, v->installation_id, 32) ||
       put(&w, v->lineage_id, 32) || put(&w, v->operation_id, 64) || put(&w, v->authority_id, 32) ||
       put(&w, v->storage_id, 32) || put_u64(&w, (uint64_t)v->generation) ||
       put_u32(&w, (uint32_t)v->provider_kind) || put(&w, v->nonce, 32) ||
       put(&w, v->binding_digest, 32) || put(&w, v->csr_digest, 32) ||
       put(&w, v->csr_spki_digest, 32) || put(&w, v->public_bundle_digest, 32) ||
       put(&w, v->custody_binding_digest, 32) || put(&w, v->fingerprint, 32) ||
       put(&w, v->spki_digest, 32) || put(&w, v->ca_fingerprint, 32) ||
       put_u16(&w, (uint16_t)issuer_len) || put(&w, v->issuer, issuer_len) ||
       put_u16(&w, (uint16_t)ca_issuer_len) || put(&w, v->ca_issuer, ca_issuer_len) ||
       put_u16(&w, (uint16_t)serial_len) || put(&w, v->serial_norm, serial_len) ||
       put_u64(&w, (uint64_t)v->not_before_epoch) || put_u64(&w, (uint64_t)v->not_after_epoch) ||
       put_u32(&w, (uint32_t)v->ciphertext_len) || put(&w, v->ciphertext, v->ciphertext_len);
   if (bad)
   {
      return -1;
   }
   *out_len = total;
   return 0;
}

static int copy_fixed(reader_t *r, char *out, size_t n)
{
   const uint8_t *p;
   if (get(r, &p, n))
      return -1;
   memcpy(out, p, n);
   out[n] = 0;
   return exact_hex(out, n) ? 0 : -1;
}

int kb_management_cert_candidate_decode(const void *input, size_t len,
                                        kb_management_cert_candidate_view_t *out)
{
   if (!out || overlap(input, len, out, sizeof(*out)))
      return -1;
   memset(out, 0, sizeof(*out));
   if (!input || len > KB_MANAGEMENT_CERT_CANDIDATE_MAX)
      return -1;
   reader_t r = {input, len};
   kb_management_cert_candidate_view_t v = {0};
   const uint8_t *p;
   uint64_t epoch;
   uint16_t n16;
   uint32_t n32, kind;
   if (exact_magic(&r, candidate_magic, sizeof(candidate_magic) - 1) ||
       copy_fixed(&r, v.installation_id, 32) || copy_fixed(&r, v.lineage_id, 32) ||
       copy_fixed(&r, v.operation_id, 64) || copy_fixed(&r, v.authority_id, 32) ||
       copy_fixed(&r, v.storage_id, 32) || get_u64(&r, &epoch) || epoch < 1 || epoch > INT64_MAX ||
       get_u32(&r, &kind) || kind < KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 ||
       kind > KB_WORKLOAD_PROVIDER_PKCS11_V1)
      return -1;
   v.generation = (int64_t)epoch;
   v.provider_kind = (kb_workload_provider_kind_t)kind;
#define TAKE32(field) (get(&r, &p, 32) || (memcpy((field), p, 32), 0))
   if (TAKE32(v.nonce) || TAKE32(v.binding_digest) || TAKE32(v.csr_digest) ||
       TAKE32(v.csr_spki_digest) || TAKE32(v.public_bundle_digest) ||
       TAKE32(v.custody_binding_digest) || TAKE32(v.fingerprint) || TAKE32(v.spki_digest) ||
       TAKE32(v.ca_fingerprint))
      return -1;
#undef TAKE32
   if (get_u16(&r, &n16) || !n16 || n16 > DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX ||
       get(&r, &p, n16))
      return -1;
   memcpy(v.issuer, p, n16);
   v.issuer[n16] = 0;
   size_t checked;
   if (!printable(v.issuer, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX, &checked) || checked != n16 ||
       get_u16(&r, &n16) || !n16 || n16 > DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX ||
       get(&r, &p, n16))
      return -1;
   memcpy(v.ca_issuer, p, n16);
   v.ca_issuer[n16] = 0;
   if (!printable(v.ca_issuer, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX, &checked) ||
       checked != n16 || get_u16(&r, &n16) || !n16 ||
       n16 > DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX || get(&r, &p, n16))
      return -1;
   memcpy(v.serial_norm, p, n16);
   v.serial_norm[n16] = 0;
   if (!exact_hex(v.serial_norm, n16) || get_u64(&r, &epoch) || epoch < 1 || epoch > INT64_MAX)
      return -1;
   v.not_before_epoch = (int64_t)epoch;
   if (get_u64(&r, &epoch) || epoch <= (uint64_t)v.not_before_epoch || epoch > INT64_MAX)
      return -1;
   v.not_after_epoch = (int64_t)epoch;
   if (get_u32(&r, &n32) || !n32 || n32 > KB_WORKLOAD_WRAP_CAP || get(&r, &p, n32) || r.left)
      return -1;
   v.ciphertext = p;
   v.ciphertext_len = n32;
   *out = v;
   return 0;
}

int kb_management_cert_manifest_encode(const kb_management_cert_manifest_t *v, uint8_t *out,
                                       size_t cap, size_t *out_len)
{
   size_t total = sizeof(manifest_magic) - 1 + 64 + 8 + 32;
   if (!v || !out || !out_len || overlap(out, cap, v, sizeof(*v)) ||
       overlap(out, cap, out_len, sizeof(*out_len)) ||
       overlap(out_len, sizeof(*out_len), v, sizeof(*v)))
      return -1;
   *out_len = 0;
   if (cap)
      memset(out, 0, cap);
   if (cap < total || !exact_hex(v->operation_id, 64) || v->generation < 1)
      return -1;
   writer_t w = {out, cap};
   if (put(&w, manifest_magic, sizeof(manifest_magic) - 1) || put(&w, v->operation_id, 64) ||
       put_u64(&w, (uint64_t)v->generation) || put(&w, v->public_bundle_digest, 32))
      return -1;
   *out_len = total;
   return 0;
}

int kb_management_cert_manifest_decode(const void *input, size_t len,
                                       kb_management_cert_manifest_t *out)
{
   if (!out || overlap(input, len, out, sizeof(*out)))
      return -1;
   memset(out, 0, sizeof(*out));
   if (!input || !len)
      return -1;
   reader_t r = {input, len};
   kb_management_cert_manifest_t v = {0};
   uint64_t generation;
   const uint8_t *p;
   if (exact_magic(&r, manifest_magic, sizeof(manifest_magic) - 1) ||
       copy_fixed(&r, v.operation_id, 64) || get_u64(&r, &generation) || generation < 1 ||
       generation > INT64_MAX || get(&r, &p, 32) || r.left)
      return -1;
   v.generation = (int64_t)generation;
   memcpy(v.public_bundle_digest, p, 32);
   *out = v;
   return 0;
}

int kb_management_cert_pending_encode(const kb_management_cert_pending_manifest_t *v, uint8_t *out,
                                      size_t cap, size_t *out_len)
{
   const size_t total = sizeof(pending_magic) - 1 + 32 + 32 + 64 + 32 + 8 + 4 + 32 + 32;
   if (!v || !out || !out_len || overlap(out, cap, v, sizeof(*v)) ||
       overlap(out, cap, out_len, sizeof(*out_len)) ||
       overlap(out_len, sizeof(*out_len), v, sizeof(*v)))
      return -1;
   *out_len = 0;
   if (cap)
      memset(out, 0, cap);
   if (cap < total || !exact_hex(v->installation_id, 32) || !exact_hex(v->lineage_id, 32) ||
       !exact_hex(v->operation_id, 64) || !exact_hex(v->authority_id, 32) || v->generation < 1 ||
       (v->issue_kind != KB_MANAGEMENT_CERT_ISSUE_INITIAL &&
        v->issue_kind != KB_MANAGEMENT_CERT_ISSUE_RENEWAL))
      return -1;
   writer_t w = {out, cap};
   if (put(&w, pending_magic, sizeof(pending_magic) - 1) || put(&w, v->installation_id, 32) ||
       put(&w, v->lineage_id, 32) || put(&w, v->operation_id, 64) || put(&w, v->authority_id, 32) ||
       put_u64(&w, (uint64_t)v->generation) || put_u32(&w, (uint32_t)v->issue_kind) ||
       put(&w, v->binding_digest, 32) || put(&w, v->intent_record_digest, 32))
      return -1;
   *out_len = total;
   return 0;
}

int kb_management_cert_pending_decode(const void *input, size_t len,
                                      kb_management_cert_pending_manifest_t *out)
{
   const size_t exact_len = sizeof(pending_magic) - 1 + 32 + 32 + 64 + 32 + 8 + 4 + 32 + 32;
   if (!out || overlap(input, len, out, sizeof(*out)))
      return -1;
   memset(out, 0, sizeof(*out));
   if (!input || len != exact_len)
      return -1;
   reader_t r = {input, len};
   kb_management_cert_pending_manifest_t v = {0};
   uint64_t generation;
   uint32_t kind;
   const uint8_t *p;
   if (exact_magic(&r, pending_magic, sizeof(pending_magic) - 1) ||
       copy_fixed(&r, v.installation_id, 32) || copy_fixed(&r, v.lineage_id, 32) ||
       copy_fixed(&r, v.operation_id, 64) || copy_fixed(&r, v.authority_id, 32) ||
       get_u64(&r, &generation) || generation < 1 || generation > INT64_MAX || get_u32(&r, &kind) ||
       (kind != KB_MANAGEMENT_CERT_ISSUE_INITIAL && kind != KB_MANAGEMENT_CERT_ISSUE_RENEWAL) ||
       get(&r, &p, 32))
      return -1;
   v.generation = (int64_t)generation;
   v.issue_kind = (kb_management_cert_issue_kind_t)kind;
   memcpy(v.binding_digest, p, 32);
   if (get(&r, &p, 32) || r.left)
      return -1;
   memcpy(v.intent_record_digest, p, 32);
   *out = v;
   return 0;
}
