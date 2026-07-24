#include "vault_witness_record.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>

static const uint8_t record_magic[8] = {'A', 'I', 'M', 'W', 'T', 'N', 'S', '1'};

/* Wire layout: fixed 140-byte header, then 8 length-prefixed strings. */
#define WIRE_HEADER_LEN 140
#define WIRE_OFF_MAGIC 0
#define WIRE_OFF_VERMAJOR 8
#define WIRE_OFF_VERMINOR 10
#define WIRE_OFF_TOTALLEN 12
#define WIRE_OFF_SOURCE 16
#define WIRE_OFF_FLAGS 17
#define WIRE_OFF_RESERVED 18
#define WIRE_OFF_SEAL_EPOCH 20
#define WIRE_OFF_FENCE 28
#define WIRE_OFF_SHARD_SEQ 36
#define WIRE_OFF_SOURCE_HASH 44
#define WIRE_OFF_SOURCE_PRED 76
#define WIRE_OFF_WITNESS_PRED 108

#define FLAG_HAS_SOURCE_PRED 0x01u
#define FLAG_IS_FIRST 0x02u

static void put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
   for (unsigned i = 0; i < 8; i++)
      p[i] = (uint8_t)(v >> (56U - 8U * i));
}

static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_u64(const uint8_t *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; i++)
      v = (v << 8) | p[i];
   return v;
}

static int all_zero(const uint8_t *p, size_t n)
{
   uint8_t any = 0;
   for (size_t i = 0; i < n; i++)
      any |= p[i];
   return any == 0;
}

/* Append pack_text(s) = int4send(len) || bytes, matching org_vault_rewrap_pack_bytes.
 * Advances *off; returns 0 on success, -1 if it would overflow `cap`. */
static int pack_text(uint8_t *buf, size_t cap, size_t *off, const char *s, size_t len)
{
   if (*off + 4 + len < *off || *off + 4 + len > cap)
      return -1;
   put_u32(buf + *off, (uint32_t)len);
   *off += 4;
   if (len)
      memcpy(buf + *off, s, len);
   *off += len;
   return 0;
}

int vault_witness_shard_key_hash(const char *tenant, const char *provider, uint8_t out[8])
{
   if (!out)
      return -1;
   OPENSSL_cleanse(out, 8);
   if (!tenant || !provider)
      return -1;
   size_t tl = strlen(tenant), pl = strlen(provider);
   if (tl == 0 || tl > VAULT_WITNESS_TENANT_MAX || pl > VAULT_WITNESS_PROVIDER_MAX)
      return -1;
   uint8_t buf[8 + VAULT_WITNESS_TENANT_MAX + VAULT_WITNESS_PROVIDER_MAX];
   size_t off = 0;
   if (pack_text(buf, sizeof buf, &off, tenant, tl) != 0 ||
       pack_text(buf, sizeof buf, &off, provider, pl) != 0)
      return -1;
   uint8_t full[32];
   if (!SHA256(buf, off, full))
      return -1;
   memcpy(out, full, 8);
   return 0;
}

int vault_witness_genesis_sentinel(const char *tenant, const char *provider, uint8_t out[32])
{
   if (!out)
      return -1;
   OPENSSL_cleanse(out, 32);
   if (!tenant || !provider)
      return -1;
   size_t tl = strlen(tenant), pl = strlen(provider);
   if (tl == 0 || tl > VAULT_WITNESS_TENANT_MAX || pl > VAULT_WITNESS_PROVIDER_MAX)
      return -1;
   uint8_t buf[sizeof(VAULT_WITNESS_GENESIS_LABEL) + 8 + VAULT_WITNESS_TENANT_MAX +
               VAULT_WITNESS_PROVIDER_MAX];
   size_t off = 0;
   size_t label_len = strlen(VAULT_WITNESS_GENESIS_LABEL);
   memcpy(buf, VAULT_WITNESS_GENESIS_LABEL, label_len);
   off = label_len;
   if (pack_text(buf, sizeof buf, &off, tenant, tl) != 0 ||
       pack_text(buf, sizeof buf, &off, provider, pl) != 0)
      return -1;
   if (!SHA256(buf, off, out))
   {
      OPENSSL_cleanse(out, 32);
      return -1;
   }
   return 0;
}

/* A NUL-free bounded C string of length in [0,cap]. Shard-key components must be
 * non-empty; callers pass min==1 for those. */
static int str_ok(const char *s, size_t cap, size_t min, size_t *out_len)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, cap + 1);
   if (n > cap || n < min)
      return 0;
   *out_len = n;
   return 1;
}

/* Semantic validation independent of wire framing. Populates field lengths. */
static int record_valid(const vault_witness_record_t *r)
{
   size_t n;
   if (!r)
      return 0;
   if (r->source != VAULT_WITNESS_SRC_AUDIT && r->source != VAULT_WITNESS_SRC_REWRAP &&
       r->source != VAULT_WITNESS_SRC_OPEN)
      return 0;
   if (r->shard_seq == 0)
      return 0;
   if (!str_ok(r->source_id, VAULT_WITNESS_SOURCE_ID_MAX, 1, &n) ||
       !str_ok(r->tenant, VAULT_WITNESS_TENANT_MAX, 1, &n) ||
       !str_ok(r->provider, VAULT_WITNESS_PROVIDER_MAX, 1, &n) ||
       !str_ok(r->request_id, VAULT_WITNESS_REQUEST_ID_MAX, 0, &n) ||
       !str_ok(r->principal, VAULT_WITNESS_PRINCIPAL_MAX, 0, &n) ||
       !str_ok(r->provider_cred, VAULT_WITNESS_PROVIDER_CRED_MAX, 0, &n) ||
       !str_ok(r->group_id, VAULT_WITNESS_GROUP_ID_MAX, 0, &n) ||
       !str_ok(r->timestamp, VAULT_WITNESS_TIMESTAMP_MAX, 1, &n))
      return 0;

   /* Only the audit ledger carries a real source predecessor. */
   if (r->has_source_pred)
   {
      if (r->source != VAULT_WITNESS_SRC_AUDIT)
         return 0;
   }
   else if (!all_zero(r->source_pred_hash, 32))
      return 0;

   /* is_first_in_shard is exactly shard_seq == 1, and pins the witness predecessor
    * to the shard-bound genesis sentinel. */
   if (r->is_first_in_shard != (r->shard_seq == 1))
      return 0;
   uint8_t genesis[32];
   if (vault_witness_genesis_sentinel(r->tenant, r->provider, genesis) != 0)
      return 0;
   int matches_genesis = CRYPTO_memcmp(r->witness_pred_hash, genesis, 32) == 0;
   OPENSSL_cleanse(genesis, sizeof genesis);
   if (r->is_first_in_shard != matches_genesis)
      return 0;

   return 1;
}

int vault_witness_record_digest(const vault_witness_record_t *r, uint8_t digest[32])
{
   if (!digest)
      return -1;
   OPENSSL_cleanse(digest, 32);
   if (!record_valid(r))
      return -1;

   uint8_t buf[4096];
   size_t off = 0;
   size_t label_len = strlen(VAULT_WITNESS_DIGEST_LABEL);
   if (pack_text(buf, sizeof buf, &off, VAULT_WITNESS_DIGEST_LABEL, label_len) != 0)
      return -1;
   buf[off++] = (uint8_t)r->source;
   if (pack_text(buf, sizeof buf, &off, r->source_id, strlen(r->source_id)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->tenant, strlen(r->tenant)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->provider, strlen(r->provider)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->request_id, strlen(r->request_id)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->principal, strlen(r->principal)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->provider_cred, strlen(r->provider_cred)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->group_id, strlen(r->group_id)) != 0 ||
       pack_text(buf, sizeof buf, &off, r->timestamp, strlen(r->timestamp)) != 0)
      return -1;
   if (off + 32 + 1 + 32 + 32 + 24 > sizeof buf)
      return -1;
   memcpy(buf + off, r->source_hash, 32);
   off += 32;
   buf[off++] = r->has_source_pred ? 1 : 0;
   memcpy(buf + off, r->source_pred_hash, 32);
   off += 32;
   memcpy(buf + off, r->witness_pred_hash, 32);
   off += 32;
   put_u64(buf + off, r->seal_epoch);
   off += 8;
   put_u64(buf + off, r->fencing_token);
   off += 8;
   put_u64(buf + off, r->shard_seq);
   off += 8;

   int ok = SHA256(buf, off, digest) != NULL;
   OPENSSL_cleanse(buf, sizeof buf);
   if (!ok)
   {
      OPENSSL_cleanse(digest, 32);
      return -1;
   }
   return 0;
}

/* Append a u16-length-prefixed string to the wire buffer. */
static int wire_put_str(uint8_t *out, size_t cap, size_t *off, const char *s)
{
   size_t len = strlen(s);
   if (len > 0xFFFF || *off + 2 + len < *off || *off + 2 + len > cap)
      return -1;
   put_u16(out + *off, (uint16_t)len);
   *off += 2;
   memcpy(out + *off, s, len);
   *off += len;
   return 0;
}

int vault_witness_record_encode(const vault_witness_record_t *r, uint8_t *out, size_t cap,
                                size_t *out_len)
{
   if (!out || !out_len)
      return -1;
   *out_len = 0;
   if (!record_valid(r) || cap < WIRE_HEADER_LEN)
   {
      if (cap)
         OPENSSL_cleanse(out, cap);
      return -1;
   }

   uint8_t hdr[WIRE_HEADER_LEN];
   memset(hdr, 0, sizeof hdr);
   memcpy(hdr + WIRE_OFF_MAGIC, record_magic, sizeof record_magic);
   put_u16(hdr + WIRE_OFF_VERMAJOR, 1);
   put_u16(hdr + WIRE_OFF_VERMINOR, 0);
   hdr[WIRE_OFF_SOURCE] = (uint8_t)r->source;
   hdr[WIRE_OFF_FLAGS] =
       (uint8_t)((r->has_source_pred ? FLAG_HAS_SOURCE_PRED : 0) |
                 (r->is_first_in_shard ? FLAG_IS_FIRST : 0));
   put_u64(hdr + WIRE_OFF_SEAL_EPOCH, r->seal_epoch);
   put_u64(hdr + WIRE_OFF_FENCE, r->fencing_token);
   put_u64(hdr + WIRE_OFF_SHARD_SEQ, r->shard_seq);
   memcpy(hdr + WIRE_OFF_SOURCE_HASH, r->source_hash, 32);
   memcpy(hdr + WIRE_OFF_SOURCE_PRED, r->source_pred_hash, 32);
   memcpy(hdr + WIRE_OFF_WITNESS_PRED, r->witness_pred_hash, 32);

   /* Body first (into out past the header) so total_len can be stamped. */
   size_t off = WIRE_HEADER_LEN;
   if (wire_put_str(out, cap, &off, r->source_id) != 0 ||
       wire_put_str(out, cap, &off, r->tenant) != 0 ||
       wire_put_str(out, cap, &off, r->provider) != 0 ||
       wire_put_str(out, cap, &off, r->request_id) != 0 ||
       wire_put_str(out, cap, &off, r->principal) != 0 ||
       wire_put_str(out, cap, &off, r->provider_cred) != 0 ||
       wire_put_str(out, cap, &off, r->group_id) != 0 ||
       wire_put_str(out, cap, &off, r->timestamp) != 0)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   if (off > 0xFFFFFFFFu)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   put_u32(hdr + WIRE_OFF_TOTALLEN, (uint32_t)off);
   memcpy(out, hdr, WIRE_HEADER_LEN);
   *out_len = off;
   return 0;
}

/* Read a u16-length-prefixed string into `dst` (cap-bounded, NUL-free). */
static int wire_get_str(const uint8_t *wire, size_t wire_len, size_t *off, char *dst, size_t cap)
{
   if (*off + 2 > wire_len)
      return -1;
   size_t len = get_u16(wire + *off);
   *off += 2;
   if (len > cap || *off + len > wire_len)
      return -1;
   if (memchr(wire + *off, 0, len) != NULL) /* text fields carry no embedded NUL */
      return -1;
   memcpy(dst, wire + *off, len);
   dst[len] = '\0';
   *off += len;
   return 0;
}

int vault_witness_record_decode(const uint8_t *wire, size_t wire_len, vault_witness_record_t *r)
{
   if (!r)
      return -1;
   OPENSSL_cleanse(r, sizeof *r);
   if (!wire || wire_len < WIRE_HEADER_LEN || wire_len > VAULT_WITNESS_RECORD_MAX ||
       CRYPTO_memcmp(wire + WIRE_OFF_MAGIC, record_magic, sizeof record_magic) != 0 ||
       get_u16(wire + WIRE_OFF_VERMAJOR) != 1 || get_u16(wire + WIRE_OFF_VERMINOR) != 0 ||
       get_u32(wire + WIRE_OFF_TOTALLEN) != wire_len || get_u16(wire + WIRE_OFF_RESERVED) != 0)
      return -1;

   uint8_t flags = wire[WIRE_OFF_FLAGS];
   if (flags & ~(FLAG_HAS_SOURCE_PRED | FLAG_IS_FIRST))
      return -1;

   vault_witness_record_t tmp;
   memset(&tmp, 0, sizeof tmp);
   tmp.source = (vault_witness_source_t)wire[WIRE_OFF_SOURCE];
   tmp.has_source_pred = (flags & FLAG_HAS_SOURCE_PRED) ? 1 : 0;
   tmp.is_first_in_shard = (flags & FLAG_IS_FIRST) ? 1 : 0;
   tmp.seal_epoch = get_u64(wire + WIRE_OFF_SEAL_EPOCH);
   tmp.fencing_token = get_u64(wire + WIRE_OFF_FENCE);
   tmp.shard_seq = get_u64(wire + WIRE_OFF_SHARD_SEQ);
   memcpy(tmp.source_hash, wire + WIRE_OFF_SOURCE_HASH, 32);
   memcpy(tmp.source_pred_hash, wire + WIRE_OFF_SOURCE_PRED, 32);
   memcpy(tmp.witness_pred_hash, wire + WIRE_OFF_WITNESS_PRED, 32);

   size_t off = WIRE_HEADER_LEN;
   if (wire_get_str(wire, wire_len, &off, tmp.source_id, VAULT_WITNESS_SOURCE_ID_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.tenant, VAULT_WITNESS_TENANT_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.provider, VAULT_WITNESS_PROVIDER_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.request_id, VAULT_WITNESS_REQUEST_ID_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.principal, VAULT_WITNESS_PRINCIPAL_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.provider_cred, VAULT_WITNESS_PROVIDER_CRED_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.group_id, VAULT_WITNESS_GROUP_ID_MAX) != 0 ||
       wire_get_str(wire, wire_len, &off, tmp.timestamp, VAULT_WITNESS_TIMESTAMP_MAX) != 0)
   {
      OPENSSL_cleanse(&tmp, sizeof tmp);
      return -1;
   }
   if (off != wire_len || !record_valid(&tmp)) /* no trailing bytes; full semantic check */
   {
      OPENSSL_cleanse(&tmp, sizeof tmp);
      return -1;
   }
   *r = tmp;
   OPENSSL_cleanse(&tmp, sizeof tmp);
   return 0;
}

int vault_witness_record_equal(const vault_witness_record_t *a, const vault_witness_record_t *b)
{
   uint8_t aw[VAULT_WITNESS_RECORD_MAX], bw[VAULT_WITNESS_RECORD_MAX];
   size_t al = 0, bl = 0;
   int valid = a && b && vault_witness_record_encode(a, aw, sizeof aw, &al) == 0 &&
               vault_witness_record_encode(b, bw, sizeof bw, &bl) == 0;
   int equal = valid && al == bl && CRYPTO_memcmp(aw, bw, al) == 0;
   OPENSSL_cleanse(aw, sizeof aw);
   OPENSSL_cleanse(bw, sizeof bw);
   return equal;
}
