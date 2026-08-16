/* Parity tests for descriptor-owned DB2 sketch support. */
#include "sketch.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

uint64_t db2_support_sketch_fnv1a(const void *data, size_t len);
void db2_support_sketch_bloom_init(sketch_bloom_t *b);
void db2_support_sketch_minhash_init(sketch_minhash_t *sig);
uint64_t db2_support_sketch_lsh_band_hash(const sketch_minhash_t *sig, int band);
void db2_support_sketch_count_min_init(sketch_count_min_t *cm);
void db2_support_sketch_hll_init(sketch_hll_t *hll);
void db2_support_sketch_hll_add_hash(sketch_hll_t *hll, uint64_t h);

static void test_fnv_parity(void)
{
   static const unsigned char binary[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
   assert(db2_support_sketch_fnv1a("", 0) == UINT64_C(0xcbf29ce484222325));
   assert(db2_support_sketch_fnv1a("hello", 5) == UINT64_C(0xa430d84680aabd0b));
   assert(db2_support_sketch_fnv1a(binary, sizeof(binary)) == UINT64_C(0xa5bcd1d1065f84b6));
   assert(db2_support_sketch_fnv1a("", 0) == sketch_fnv1a("", 0));
   assert(db2_support_sketch_fnv1a("hello", 5) == sketch_fnv1a("hello", 5));
   assert(db2_support_sketch_fnv1a(binary, sizeof(binary)) == sketch_fnv1a(binary, sizeof(binary)));
   assert(db2_support_sketch_fnv1a(NULL, 0) == sketch_fnv1a(NULL, 0));
}

static void test_init_parity(void)
{
   sketch_bloom_t bloom_a, bloom_b;
   static sketch_count_min_t count_a, count_b;
   sketch_minhash_t minhash_a, minhash_b;
   sketch_hll_t hll_a, hll_b;

   memset(&bloom_a, 0xa5, sizeof(bloom_a));
   bloom_b = bloom_a;
   sketch_bloom_init(&bloom_a);
   db2_support_sketch_bloom_init(&bloom_b);
   assert(memcmp(&bloom_a, &bloom_b, sizeof(bloom_a)) == 0);
   assert(bloom_b.item_count == 0);

   memset(&count_a, 0xa5, sizeof(count_a));
   memcpy(&count_b, &count_a, sizeof(count_a));
   sketch_count_min_init(&count_a);
   db2_support_sketch_count_min_init(&count_b);
   assert(memcmp(&count_a, &count_b, sizeof(count_a)) == 0);
   assert(count_b.item_count == 0);

   memset(&minhash_a, 0xa5, sizeof(minhash_a));
   minhash_b = minhash_a;
   sketch_minhash_init(&minhash_a);
   db2_support_sketch_minhash_init(&minhash_b);
   assert(memcmp(&minhash_a, &minhash_b, sizeof(minhash_a)) == 0);
   for (size_t i = 0; i < SKETCH_MINHASH_PERMUTATIONS; i++)
      assert(minhash_b.values[i] == UINT64_MAX);

   memset(&hll_a, 0xa5, sizeof(hll_a));
   hll_b = hll_a;
   sketch_hll_init(&hll_a);
   db2_support_sketch_hll_init(&hll_b);
   assert(memcmp(&hll_a, &hll_b, sizeof(hll_a)) == 0);
   assert(hll_b.item_count == 0);
}

static void test_lsh_parity(void)
{
   sketch_minhash_t signature;
   for (size_t i = 0; i < SKETCH_MINHASH_PERMUTATIONS; i++)
      signature.values[i] = UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)i;
   for (int band = 0; band < SKETCH_LSH_BANDS; band++)
      assert(db2_support_sketch_lsh_band_hash(&signature, band) ==
             sketch_lsh_band_hash(&signature, band));
   assert(db2_support_sketch_lsh_band_hash(NULL, 0) == 0);
   assert(db2_support_sketch_lsh_band_hash(&signature, -1) == 0);
   assert(db2_support_sketch_lsh_band_hash(&signature, SKETCH_LSH_BANDS) == 0);
}

static void test_hll_add_parity(void)
{
   static const uint64_t hashes[] = {
       0, 1, UINT64_C(0x1000), UINT64_C(0x8000000000000000), UINT64_MAX,
   };
   sketch_hll_t a, b;
   sketch_hll_init(&a);
   db2_support_sketch_hll_init(&b);
   for (size_t i = 0; i < sizeof(hashes) / sizeof(hashes[0]); i++)
   {
      sketch_hll_add_hash(&a, hashes[i]);
      db2_support_sketch_hll_add_hash(&b, hashes[i]);
   }
   assert(memcmp(&a, &b, sizeof(a)) == 0);
   assert(b.item_count == sizeof(hashes) / sizeof(hashes[0]));
   db2_support_sketch_hll_add_hash(NULL, 7);
}

int main(void)
{
   test_fnv_parity();
   test_init_parity();
   test_lsh_parity();
   test_hll_add_parity();
   return 0;
}
