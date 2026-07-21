#include "modules/vault/vault_reseal_receipt.h"

#include <assert.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int all_zero(const void *ptr, size_t len)
{
   const uint8_t *p = ptr;
   uint8_t any = 0;
   for (size_t i = 0; i < len; i++)
      any |= p[i];
   return any == 0;
}

static vault_tpm2_reseal_receipt_t fixture(void)
{
   vault_tpm2_reseal_receipt_t r;
   memset(&r, 0, sizeof(r));
   for (size_t i = 0; i < sizeof(r.operation_id); i++)
      r.operation_id[i] = (uint8_t)i;
   r.old_generation = UINT64_C(0x0102030405060708);
   r.new_generation = r.old_generation + 1;
   for (size_t i = 0; i < 32; i++)
   {
      r.predecessor_digest[i] = (uint8_t)(0x10 + i);
      r.capsule_digest[i] = (uint8_t)(0x30 + i);
      r.future_digest[i] = (uint8_t)(0x50 + i);
      r.new_kek_digest[i] = (uint8_t)(0x70 + i);
      r.manifest_digest[i] = (uint8_t)(0x90 + i);
   }
   return r;
}

static void expected_wire(uint8_t out[VAULT_RESEAL_RECEIPT_V1_LEN])
{
   static const uint8_t header[16] = {'A', 'I', 'M', 'R', 'S', 'E', 'A', 'L',
                                      0,   1,   0,   0,   0,   0,   0,   192};
   static const uint8_t generations[16] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 9};
   memcpy(out, header, sizeof(header));
   for (size_t i = 0; i < 16; i++)
      out[16 + i] = (uint8_t)i;
   memcpy(out + 32, generations, sizeof(generations));
   for (size_t i = 0; i < 32; i++)
   {
      out[48 + i] = (uint8_t)(0x10 + i);
      out[80 + i] = (uint8_t)(0x30 + i);
      out[112 + i] = (uint8_t)(0x50 + i);
      out[144 + i] = (uint8_t)(0x70 + i);
      out[176 + i] = (uint8_t)(0x90 + i);
   }
}

static void test_known_answer(void)
{
   static const uint8_t expected_digest[32] = {0xa1, 0xed, 0xff, 0xbe, 0xc8, 0x1c, 0x28, 0x1c,
                                               0x4d, 0x91, 0xf7, 0x90, 0xf5, 0xf0, 0x87, 0x35,
                                               0xa2, 0x98, 0x3b, 0x0c, 0x32, 0x4c, 0xc1, 0x60,
                                               0xed, 0xa6, 0x01, 0xab, 0xe0, 0x08, 0x60, 0x4a};
   vault_tpm2_reseal_receipt_t r = fixture(), decoded;
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN], expected[VAULT_RESEAL_RECEIPT_V1_LEN], digest[32];
   expected_wire(expected);
   assert(vault_reseal_receipt_encode(&r, wire) == 0);
   assert(CRYPTO_memcmp(wire, expected, sizeof(wire)) == 0);
   assert(vault_reseal_receipt_decode(wire, sizeof(wire), &decoded) == 0);
   assert(vault_reseal_receipt_equal(&r, &decoded));
   assert(vault_reseal_receipt_digest(wire, digest) == 0);
   assert(CRYPTO_memcmp(digest, expected_digest, sizeof(digest)) == 0);
}

static void test_strict_decode_and_zeroing(void)
{
   vault_tpm2_reseal_receipt_t original = fixture(), out;
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN + 1];
   assert(vault_reseal_receipt_encode(&original, wire) == 0);

   for (size_t len = 0; len < VAULT_RESEAL_RECEIPT_V1_LEN; len++)
   {
      memset(&out, 0xa5, sizeof(out));
      assert(vault_reseal_receipt_decode(wire, len, &out) == -1 && all_zero(&out, sizeof(out)));
   }
   wire[VAULT_RESEAL_RECEIPT_V1_LEN] = 0;
   memset(&out, 0xa5, sizeof(out));
   assert(vault_reseal_receipt_decode(wire, sizeof(wire), &out) == -1 &&
          all_zero(&out, sizeof(out)));

   static const size_t header_mutations[] = {0, 8, 9, 10, 11, 12, 13, 14, 15, 32, 40};
   for (size_t i = 0; i < sizeof(header_mutations) / sizeof(header_mutations[0]); i++)
   {
      size_t at = header_mutations[i];
      wire[at] ^= 0x80;
      memset(&out, 0xa5, sizeof(out));
      assert(vault_reseal_receipt_decode(wire, VAULT_RESEAL_RECEIPT_V1_LEN, &out) == -1);
      assert(all_zero(&out, sizeof(out)));
      wire[at] ^= 0x80;
   }

   uint8_t bad_digest[32];
   wire[10] = 1;
   memset(bad_digest, 0xa5, sizeof(bad_digest));
   assert(vault_reseal_receipt_digest(wire, bad_digest) == -1 &&
          all_zero(bad_digest, sizeof(bad_digest)));
   wire[10] = 0;

   union
   {
      vault_tpm2_reseal_receipt_t receipt;
      uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
   } overlap;
   overlap.receipt = original;
   uint8_t before[VAULT_RESEAL_RECEIPT_V1_LEN];
   memcpy(before, overlap.wire, sizeof(before));
   assert(vault_reseal_receipt_encode(&overlap.receipt, overlap.wire) == -1);
   assert(CRYPTO_memcmp(overlap.wire, before, sizeof(before)) == 0);
   memcpy(overlap.wire, wire, VAULT_RESEAL_RECEIPT_V1_LEN);
   memcpy(before, overlap.wire, sizeof(before));
   assert(vault_reseal_receipt_decode(overlap.wire, sizeof(overlap.wire), &overlap.receipt) == -1);
   assert(CRYPTO_memcmp(overlap.wire, before, sizeof(before)) == 0);

   memcpy(wire, before, VAULT_RESEAL_RECEIPT_V1_LEN);
   memcpy(before, wire, sizeof(before));
   assert(vault_reseal_receipt_digest(wire, wire + 64) == -1);
   assert(CRYPTO_memcmp(wire, before, sizeof(before)) == 0);
}

static void test_operation_id_overlap_and_failure_zeroing(void)
{
   uint8_t alias[VAULT_RESEAL_OPERATION_HEX_LEN + 1], before[sizeof(alias)];
   memset(alias, 0x5a, sizeof(alias));
   for (size_t i = 0; i < VAULT_RESEAL_OPERATION_ID_LEN; i++)
      alias[i] = (uint8_t)i;
   memcpy(before, alias, sizeof(alias));
   assert(vault_reseal_operation_id_to_hex(alias, (char *)alias) == -1);
   assert(CRYPTO_memcmp(alias, before, sizeof(alias)) == 0);

   memcpy(alias, "000102030405060708090a0b0c0d0e0f", sizeof(alias));
   memcpy(before, alias, sizeof(alias));
   assert(vault_reseal_operation_id_from_hex((const char *)alias, alias) == -1);
   assert(CRYPTO_memcmp(alias, before, sizeof(alias)) == 0);

   char hex[VAULT_RESEAL_OPERATION_HEX_LEN + 1];
   memset(hex, 0xa5, sizeof(hex));
   assert(vault_reseal_operation_id_to_hex(NULL, hex) == -1 && all_zero(hex, sizeof(hex)));
   uint8_t id[VAULT_RESEAL_OPERATION_ID_LEN];
   memset(id, 0xa5, sizeof(id));
   assert(vault_reseal_operation_id_from_hex("bad", id) == -1 && all_zero(id, sizeof(id)));
}

static void test_boundaries_equality_and_operation_id(void)
{
   vault_tpm2_reseal_receipt_t a = fixture(), b = a, original = a;
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
   a.old_generation = (uint64_t)INT64_MAX - 1;
   a.new_generation = (uint64_t)INT64_MAX;
   assert(vault_reseal_receipt_encode(&a, wire) == 0);
   a.old_generation = (uint64_t)INT64_MAX;
   a.new_generation = (uint64_t)INT64_MAX + 1;
   memset(wire, 0xa5, sizeof(wire));
   assert(vault_reseal_receipt_encode(&a, wire) == -1 && all_zero(wire, sizeof(wire)));

   b.manifest_digest[31] ^= 1;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.operation_id[0] ^= 1;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.old_generation--;
   b.new_generation--;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.predecessor_digest[0] ^= 1;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.capsule_digest[0] ^= 1;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.future_digest[0] ^= 1;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.new_kek_digest[0] ^= 1;
   assert(!vault_reseal_receipt_equal(&original, &b));
   b = original;
   b.new_generation++;
   assert(!vault_reseal_receipt_equal(&original, &b));

   a = fixture();
   char hex[VAULT_RESEAL_OPERATION_HEX_LEN + 1];
   uint8_t id[VAULT_RESEAL_OPERATION_ID_LEN];
   assert(vault_reseal_operation_id_to_hex(a.operation_id, hex) == 0);
   assert(strcmp(hex, "000102030405060708090a0b0c0d0e0f") == 0);
   assert(vault_reseal_operation_id_from_hex(hex, id) == 0);
   assert(CRYPTO_memcmp(id, a.operation_id, sizeof(id)) == 0);

   static const char *bad[] = {"",
                               "000102030405060708090a0b0c0d0e0",
                               "000102030405060708090a0b0c0d0e0f0",
                               "000102030405060708090A0B0C0D0E0F",
                               "00010203-0405-0607-0809-0a0b0c0d0e0f",
                               "0x000102030405060708090a0b0c0d0e0f",
                               "000102030405060708090a0b0c0d0e0g"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      memset(id, 0xa5, sizeof(id));
      assert(vault_reseal_operation_id_from_hex(bad[i], id) == -1 && all_zero(id, sizeof(id)));
   }
}

int main(void)
{
   test_known_answer();
   test_strict_decode_and_zeroing();
   test_operation_id_overlap_and_failure_zeroing();
   test_boundaries_equality_and_operation_id();
   puts("vault_reseal_receipt: all tests passed");
   return 0;
}
