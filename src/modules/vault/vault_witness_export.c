#include "vault_witness_export.h"

#include <openssl/crypto.h>
#include <string.h>

static const uint8_t export_magic[8] = {'A', 'I', 'M', 'W', 'E', 'X', 'P', '1'};

#define OFF_MAGIC 0
#define OFF_VERSION 8
#define OFF_KIND 10
#define OFF_RESERVED 11
#define OFF_PAYLOAD_LEN 12

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

static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int kind_known(vault_witness_export_kind_t k)
{
   return k == VAULT_WITNESS_EXPORT_RECORD || k == VAULT_WITNESS_EXPORT_CHECKPOINT ||
          k == VAULT_WITNESS_EXPORT_PROOF || k == VAULT_WITNESS_EXPORT_SNAPSHOT;
}

int vault_witness_export_frame(vault_witness_export_kind_t kind, const uint8_t *payload,
                               size_t payload_len, uint8_t *out, size_t cap, size_t *out_len)
{
   if (!out || !out_len)
      return -1;
   *out_len = 0;
   if (!kind_known(kind) || (payload_len && !payload) || payload_len > 0xFFFFFFFFu)
   {
      if (cap)
         OPENSSL_cleanse(out, cap);
      return -1;
   }
   size_t need = VAULT_WITNESS_EXPORT_HEADER_LEN + payload_len;
   if (need < payload_len || cap < need)
   {
      if (cap)
         OPENSSL_cleanse(out, cap);
      return -1;
   }
   memcpy(out + OFF_MAGIC, export_magic, sizeof export_magic);
   put_u16(out + OFF_VERSION, VAULT_WITNESS_EXPORT_VERSION);
   out[OFF_KIND] = (uint8_t)kind;
   out[OFF_RESERVED] = 0;
   put_u32(out + OFF_PAYLOAD_LEN, (uint32_t)payload_len);
   if (payload_len)
      memcpy(out + VAULT_WITNESS_EXPORT_HEADER_LEN, payload, payload_len);
   *out_len = need;
   return 0;
}

vault_witness_export_parse_t vault_witness_export_parse(const uint8_t *frame, size_t frame_len,
                                                        vault_witness_export_kind_t *kind,
                                                        const uint8_t **payload,
                                                        size_t *payload_len)
{
   if (!frame || !kind || !payload || !payload_len)
      return VAULT_WITNESS_EXPORT_PARSE_MALFORMED;
   if (frame_len < VAULT_WITNESS_EXPORT_HEADER_LEN ||
       CRYPTO_memcmp(frame + OFF_MAGIC, export_magic, sizeof export_magic) != 0)
      return VAULT_WITNESS_EXPORT_PARSE_MALFORMED;
   /* Version is checked before structure so a rollout reads as a version mismatch,
    * never as corruption. */
   if (get_u16(frame + OFF_VERSION) != VAULT_WITNESS_EXPORT_VERSION)
      return VAULT_WITNESS_EXPORT_PARSE_VERSION_MISMATCH;
   if (frame[OFF_RESERVED] != 0)
      return VAULT_WITNESS_EXPORT_PARSE_MALFORMED;
   vault_witness_export_kind_t k = (vault_witness_export_kind_t)frame[OFF_KIND];
   if (!kind_known(k))
      return VAULT_WITNESS_EXPORT_PARSE_MALFORMED;
   size_t plen = get_u32(frame + OFF_PAYLOAD_LEN);
   if (plen != frame_len - VAULT_WITNESS_EXPORT_HEADER_LEN)
      return VAULT_WITNESS_EXPORT_PARSE_MALFORMED;
   *kind = k;
   *payload = plen ? frame + VAULT_WITNESS_EXPORT_HEADER_LEN : NULL;
   *payload_len = plen;
   return VAULT_WITNESS_EXPORT_PARSE_OK;
}
