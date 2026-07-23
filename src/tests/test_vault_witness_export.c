#include "modules/vault/vault_witness_export.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_frame_roundtrip(void)
{
   uint8_t payload[64];
   for (int i = 0; i < 64; i++)
      payload[i] = (uint8_t)(i * 3 + 1);

   uint8_t frame[VAULT_WITNESS_EXPORT_HEADER_LEN + 64];
   size_t len = 0;
   assert(vault_witness_export_frame(VAULT_WITNESS_EXPORT_CHECKPOINT, payload, sizeof payload, frame,
                                     sizeof frame, &len) == 0);
   assert(len == VAULT_WITNESS_EXPORT_HEADER_LEN + 64);

   vault_witness_export_kind_t kind;
   const uint8_t *p;
   size_t plen;
   assert(vault_witness_export_parse(frame, len, &kind, &p, &plen) == VAULT_WITNESS_EXPORT_PARSE_OK);
   assert(kind == VAULT_WITNESS_EXPORT_CHECKPOINT && plen == 64 && memcmp(p, payload, 64) == 0);
}

static void test_deterministic(void)
{
   uint8_t payload[20];
   memset(payload, 0xAB, sizeof payload);
   uint8_t a[64], b[64];
   size_t la = 0, lb = 0;
   assert(vault_witness_export_frame(VAULT_WITNESS_EXPORT_RECORD, payload, sizeof payload, a,
                                     sizeof a, &la) == 0);
   assert(vault_witness_export_frame(VAULT_WITNESS_EXPORT_RECORD, payload, sizeof payload, b,
                                     sizeof b, &lb) == 0);
   assert(la == lb && memcmp(a, b, la) == 0);
}

static void test_version_mismatch_distinct(void)
{
   uint8_t payload[8] = {0};
   uint8_t frame[VAULT_WITNESS_EXPORT_HEADER_LEN + 8];
   size_t len = 0;
   assert(vault_witness_export_frame(VAULT_WITNESS_EXPORT_PROOF, payload, sizeof payload, frame,
                                     sizeof frame, &len) == 0);

   /* Bump the export version -> version mismatch, not malformed. */
   frame[9] = VAULT_WITNESS_EXPORT_VERSION + 1;
   vault_witness_export_kind_t kind;
   const uint8_t *p;
   size_t plen;
   assert(vault_witness_export_parse(frame, len, &kind, &p, &plen) ==
          VAULT_WITNESS_EXPORT_PARSE_VERSION_MISMATCH);
}

static void test_malformed(void)
{
   uint8_t payload[8] = {0};
   uint8_t frame[VAULT_WITNESS_EXPORT_HEADER_LEN + 8];
   size_t len = 0;
   assert(vault_witness_export_frame(VAULT_WITNESS_EXPORT_RECORD, payload, sizeof payload, frame,
                                     sizeof frame, &len) == 0);
   vault_witness_export_kind_t kind;
   const uint8_t *p;
   size_t plen;

   /* Short frame. */
   assert(vault_witness_export_parse(frame, 15, &kind, &p, &plen) ==
          VAULT_WITNESS_EXPORT_PARSE_MALFORMED);
   /* Corrupt magic. */
   uint8_t bad[sizeof frame];
   memcpy(bad, frame, len);
   bad[0] ^= 0xFF;
   assert(vault_witness_export_parse(bad, len, &kind, &p, &plen) ==
          VAULT_WITNESS_EXPORT_PARSE_MALFORMED);
   /* Unknown kind. */
   memcpy(bad, frame, len);
   bad[10] = 9;
   assert(vault_witness_export_parse(bad, len, &kind, &p, &plen) ==
          VAULT_WITNESS_EXPORT_PARSE_MALFORMED);
   /* Payload length disagreeing with frame length. */
   memcpy(bad, frame, len);
   bad[15] = 0xFF;
   assert(vault_witness_export_parse(bad, len, &kind, &p, &plen) ==
          VAULT_WITNESS_EXPORT_PARSE_MALFORMED);
   /* Nonzero reserved. */
   memcpy(bad, frame, len);
   bad[11] = 1;
   assert(vault_witness_export_parse(bad, len, &kind, &p, &plen) ==
          VAULT_WITNESS_EXPORT_PARSE_MALFORMED);
}

static void test_empty_payload(void)
{
   uint8_t frame[VAULT_WITNESS_EXPORT_HEADER_LEN];
   size_t len = 0;
   assert(vault_witness_export_frame(VAULT_WITNESS_EXPORT_RECORD, NULL, 0, frame, sizeof frame,
                                     &len) == 0);
   assert(len == VAULT_WITNESS_EXPORT_HEADER_LEN);
   vault_witness_export_kind_t kind;
   const uint8_t *p;
   size_t plen;
   assert(vault_witness_export_parse(frame, len, &kind, &p, &plen) == VAULT_WITNESS_EXPORT_PARSE_OK);
   assert(plen == 0 && p == NULL);
}

int main(void)
{
   test_frame_roundtrip();
   test_deterministic();
   test_version_mismatch_distinct();
   test_malformed();
   test_empty_payload();
   printf("test_vault_witness_export: all passed\n");
   return 0;
}
