#include "vault_reseal_receipt.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <string.h>

static const uint8_t receipt_magic[8] = {'A', 'I', 'M', 'R', 'S', 'E', 'A', 'L'};

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

static int generations_valid(uint64_t old_generation, uint64_t new_generation)
{
   return old_generation <= (uint64_t)INT64_MAX - 1U && new_generation == old_generation + 1U;
}

static int overlaps(const void *a, size_t a_len, const void *b, size_t b_len)
{
   uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
   if (!a || !b)
      return 0;
   return ap <= bp ? bp - ap < a_len : ap - bp < b_len;
}

int vault_reseal_receipt_encode(const vault_tpm2_reseal_receipt_t *r,
                                uint8_t out[VAULT_RESEAL_RECEIPT_V1_LEN])
{
   if (!out)
      return -1;
   if (r && overlaps(r, sizeof(*r), out, VAULT_RESEAL_RECEIPT_V1_LEN))
   {
      OPENSSL_cleanse(out, VAULT_RESEAL_RECEIPT_V1_LEN);
      return -1;
   }
   OPENSSL_cleanse(out, VAULT_RESEAL_RECEIPT_V1_LEN);
   if (!r || !generations_valid(r->old_generation, r->new_generation))
      return -1;

   memcpy(out, receipt_magic, sizeof(receipt_magic));
   put_u16(out + 8, 1);
   put_u16(out + 10, 0);
   put_u32(out + 12, 192);
   memcpy(out + 16, r->operation_id, 16);
   put_u64(out + 32, r->old_generation);
   put_u64(out + 40, r->new_generation);
   memcpy(out + 48, r->predecessor_digest, 32);
   memcpy(out + 80, r->capsule_digest, 32);
   memcpy(out + 112, r->future_digest, 32);
   memcpy(out + 144, r->new_kek_digest, 32);
   memcpy(out + 176, r->manifest_digest, 32);
   return 0;
}

int vault_reseal_receipt_decode(const uint8_t *wire, size_t wire_len,
                                vault_tpm2_reseal_receipt_t *r)
{
   if (!r)
      return -1;
   if (wire && overlaps(wire, wire_len, r, sizeof(*r)))
   {
      OPENSSL_cleanse(r, sizeof(*r));
      return -1;
   }
   OPENSSL_cleanse(r, sizeof(*r));
   if (!wire || wire_len != VAULT_RESEAL_RECEIPT_V1_LEN ||
       CRYPTO_memcmp(wire, receipt_magic, sizeof(receipt_magic)) != 0 || get_u16(wire + 8) != 1 ||
       get_u16(wire + 10) != 0 || get_u32(wire + 12) != 192)
      return -1;

   vault_tpm2_reseal_receipt_t tmp;
   memset(&tmp, 0, sizeof(tmp));
   memcpy(tmp.operation_id, wire + 16, 16);
   tmp.old_generation = get_u64(wire + 32);
   tmp.new_generation = get_u64(wire + 40);
   memcpy(tmp.predecessor_digest, wire + 48, 32);
   memcpy(tmp.capsule_digest, wire + 80, 32);
   memcpy(tmp.future_digest, wire + 112, 32);
   memcpy(tmp.new_kek_digest, wire + 144, 32);
   memcpy(tmp.manifest_digest, wire + 176, 32);
   if (!generations_valid(tmp.old_generation, tmp.new_generation))
   {
      OPENSSL_cleanse(&tmp, sizeof(tmp));
      return -1;
   }
   *r = tmp;
   OPENSSL_cleanse(&tmp, sizeof(tmp));
   return 0;
}

int vault_reseal_receipt_digest(const uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN], uint8_t digest[32])
{
   if (!digest)
      return -1;
   if (wire && overlaps(wire, VAULT_RESEAL_RECEIPT_V1_LEN, digest, 32))
   {
      OPENSSL_cleanse(digest, 32);
      return -1;
   }
   OPENSSL_cleanse(digest, 32);
   if (!wire)
      return -1;
   vault_tpm2_reseal_receipt_t receipt;
   memset(&receipt, 0, sizeof(receipt));
   if (vault_reseal_receipt_decode(wire, VAULT_RESEAL_RECEIPT_V1_LEN, &receipt) != 0 ||
       !SHA256(wire, VAULT_RESEAL_RECEIPT_V1_LEN, digest))
   {
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      OPENSSL_cleanse(digest, 32);
      return -1;
   }
   OPENSSL_cleanse(&receipt, sizeof(receipt));
   return 0;
}

int vault_reseal_receipt_equal(const vault_tpm2_reseal_receipt_t *a,
                               const vault_tpm2_reseal_receipt_t *b)
{
   uint8_t aw[VAULT_RESEAL_RECEIPT_V1_LEN], bw[VAULT_RESEAL_RECEIPT_V1_LEN];
   int valid =
       a && b && vault_reseal_receipt_encode(a, aw) == 0 && vault_reseal_receipt_encode(b, bw) == 0;
   int equal = valid && CRYPTO_memcmp(aw, bw, sizeof(aw)) == 0;
   OPENSSL_cleanse(aw, sizeof(aw));
   OPENSSL_cleanse(bw, sizeof(bw));
   return equal;
}

int vault_reseal_operation_id_to_hex(const uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN],
                                     char out[VAULT_RESEAL_OPERATION_HEX_LEN + 1])
{
   static const char digits[] = "0123456789abcdef";
   if (!out)
      return -1;
   if (operation_id && overlaps(operation_id, VAULT_RESEAL_OPERATION_ID_LEN, out,
                                VAULT_RESEAL_OPERATION_HEX_LEN + 1))
   {
      OPENSSL_cleanse(out, VAULT_RESEAL_OPERATION_HEX_LEN + 1);
      return -1;
   }
   memset(out, 0, VAULT_RESEAL_OPERATION_HEX_LEN + 1);
   if (!operation_id)
      return -1;
   for (size_t i = 0; i < VAULT_RESEAL_OPERATION_ID_LEN; i++)
   {
      out[i * 2] = digits[operation_id[i] >> 4];
      out[i * 2 + 1] = digits[operation_id[i] & 0x0f];
   }
   return 0;
}

int vault_reseal_operation_id_from_hex(const char *hex,
                                       uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN])
{
   if (!operation_id)
      return -1;
   if (hex && overlaps(hex, VAULT_RESEAL_OPERATION_HEX_LEN + 1, operation_id,
                       VAULT_RESEAL_OPERATION_ID_LEN))
   {
      OPENSSL_cleanse(operation_id, VAULT_RESEAL_OPERATION_ID_LEN);
      return -1;
   }
   OPENSSL_cleanse(operation_id, VAULT_RESEAL_OPERATION_ID_LEN);
   if (!hex || strlen(hex) != VAULT_RESEAL_OPERATION_HEX_LEN)
      return -1;
   for (size_t i = 0; i < VAULT_RESEAL_OPERATION_HEX_LEN; i++)
   {
      unsigned value;
      if (hex[i] >= '0' && hex[i] <= '9')
         value = (unsigned)(hex[i] - '0');
      else if (hex[i] >= 'a' && hex[i] <= 'f')
         value = (unsigned)(hex[i] - 'a') + 10U;
      else
      {
         OPENSSL_cleanse(operation_id, VAULT_RESEAL_OPERATION_ID_LEN);
         return -1;
      }
      operation_id[i / 2] |= (uint8_t)(value << ((i & 1U) ? 0 : 4));
   }
   return 0;
}
