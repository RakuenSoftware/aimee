#include "vault_witness_proof.h"

#include <openssl/crypto.h>
#include <string.h>

static const uint8_t proof_magic[8] = {'A', 'I', 'M', 'W', 'P', 'R', 'F', '1'};

static void put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
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
static uint64_t get_u64(const uint8_t *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; i++)
      v = (v << 8) | p[i];
   return v;
}

/* Wire: magic(8) version(2) checkpoint_seq(8) tlen(2) tenant plen(2) provider
 *       sequence(8) head(32) path(DEPTH*32). */
static int proof_valid(const vault_witness_proof_t *p)
{
   if (!p || p->sequence == 0)
      return 0;
   size_t tl = strnlen(p->tenant, VAULT_WITNESS_TENANT_MAX + 1);
   size_t pl = strnlen(p->provider, VAULT_WITNESS_PROVIDER_MAX + 1);
   return tl >= 1 && tl <= VAULT_WITNESS_TENANT_MAX && pl >= 1 && pl <= VAULT_WITNESS_PROVIDER_MAX;
}

int vault_witness_proof_encode(const vault_witness_proof_t *p, uint8_t *out, size_t cap,
                               size_t *out_len)
{
   if (!out || !out_len)
      return -1;
   *out_len = 0;
   if (!proof_valid(p))
      return -1;
   size_t tl = strlen(p->tenant), pl = strlen(p->provider);
   size_t need = 8 + 2 + 8 + 2 + tl + 2 + pl + 8 + 32 + VAULT_WITNESS_SMT_DEPTH * 32;
   if (cap < need)
      return -1;
   size_t off = 0;
   memcpy(out + off, proof_magic, 8);
   off += 8;
   put_u16(out + off, 1);
   off += 2;
   put_u64(out + off, p->checkpoint_seq);
   off += 8;
   put_u16(out + off, (uint16_t)tl);
   off += 2;
   memcpy(out + off, p->tenant, tl);
   off += tl;
   put_u16(out + off, (uint16_t)pl);
   off += 2;
   memcpy(out + off, p->provider, pl);
   off += pl;
   put_u64(out + off, p->sequence);
   off += 8;
   memcpy(out + off, p->head_hash, 32);
   off += 32;
   memcpy(out + off, p->path, VAULT_WITNESS_SMT_DEPTH * 32);
   off += VAULT_WITNESS_SMT_DEPTH * 32;
   *out_len = off;
   return 0;
}

int vault_witness_proof_decode(const uint8_t *wire, size_t wire_len, vault_witness_proof_t *p)
{
   if (!p)
      return -1;
   OPENSSL_cleanse(p, sizeof *p);
   if (!wire || wire_len < 8 + 2 + 8 + 2 ||
       CRYPTO_memcmp(wire, proof_magic, 8) != 0 || get_u16(wire + 8) != 1)
      return -1;
   vault_witness_proof_t tmp;
   memset(&tmp, 0, sizeof tmp);
   size_t off = 10;
   tmp.checkpoint_seq = get_u64(wire + off);
   off += 8;
   if (off + 2 > wire_len)
      return -1;
   size_t tl = get_u16(wire + off);
   off += 2;
   if (tl == 0 || tl > VAULT_WITNESS_TENANT_MAX || off + tl > wire_len ||
       memchr(wire + off, 0, tl) != NULL)
      return -1;
   memcpy(tmp.tenant, wire + off, tl);
   off += tl;
   if (off + 2 > wire_len)
      return -1;
   size_t pl = get_u16(wire + off);
   off += 2;
   if (pl == 0 || pl > VAULT_WITNESS_PROVIDER_MAX || off + pl > wire_len ||
       memchr(wire + off, 0, pl) != NULL)
      return -1;
   memcpy(tmp.provider, wire + off, pl);
   off += pl;
   if (off + 8 + 32 + VAULT_WITNESS_SMT_DEPTH * 32 != wire_len)
      return -1;
   tmp.sequence = get_u64(wire + off);
   off += 8;
   memcpy(tmp.head_hash, wire + off, 32);
   off += 32;
   memcpy(tmp.path, wire + off, VAULT_WITNESS_SMT_DEPTH * 32);
   if (!proof_valid(&tmp))
   {
      OPENSSL_cleanse(&tmp, sizeof tmp);
      return -1;
   }
   *p = tmp;
   OPENSSL_cleanse(&tmp, sizeof tmp);
   return 0;
}

int vault_witness_proof_verify(const vault_witness_proof_t *p, const uint8_t root[32])
{
   if (!proof_valid(p) || !root)
      return 0;
   uint8_t key[8], leaf[32];
   if (vault_witness_shard_key_hash(p->tenant, p->provider, key) != 0 ||
       vault_witness_leaf_hash(p->tenant, p->provider, p->sequence, p->head_hash, leaf) != 0)
      return 0;
   return vault_witness_merkle_verify(key, leaf, p->path, root);
}
