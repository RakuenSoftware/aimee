/* Descriptor-owned DB2 process support for deterministic sketch primitives. */
#include "sketch.h"

#include <string.h>

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

uint64_t sketch_fnv1a(const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   uint64_t h = FNV_OFFSET;
   for (size_t i = 0; i < len; i++)
   {
      h ^= (uint64_t)p[i];
      h *= FNV_PRIME;
   }
   return h;
}

static uint64_t fnv1a_seeded(const void *data, size_t len, uint64_t seed)
{
   uint64_t h = sketch_fnv1a(&seed, sizeof(seed));
   const uint8_t *p = (const uint8_t *)data;
   for (size_t i = 0; i < len; i++)
   {
      h ^= (uint64_t)p[i];
      h *= FNV_PRIME;
   }
   return h;
}

void sketch_bloom_init(sketch_bloom_t *b)
{
   memset(b->bits, 0, sizeof(b->bits));
   b->item_count = 0;
}

void sketch_minhash_init(sketch_minhash_t *sig)
{
   for (int i = 0; i < SKETCH_MINHASH_PERMUTATIONS; i++)
      sig->values[i] = UINT64_MAX;
}

uint64_t sketch_lsh_band_hash(const sketch_minhash_t *sig, int band)
{
   if (!sig || band < 0 || band >= SKETCH_LSH_BANDS)
      return 0;
   return fnv1a_seeded(&sig->values[band * SKETCH_LSH_ROWS_PER_BAND],
                       sizeof(uint64_t) * SKETCH_LSH_ROWS_PER_BAND,
                       0x6a09e667f3bcc909ULL ^ (uint64_t)band);
}

void sketch_count_min_init(sketch_count_min_t *cm)
{
   memset(cm, 0, sizeof(*cm));
}

void sketch_hll_init(sketch_hll_t *hll)
{
   memset(hll, 0, sizeof(*hll));
}

static uint8_t hll_rank(uint64_t x)
{
   uint8_t rank = 1;
   int max_rank = 64 - SKETCH_HLL_PRECISION + 1;
   while (rank < max_rank && (x & (1ULL << 63)) == 0)
   {
      rank++;
      x <<= 1;
   }
   return rank;
}

void sketch_hll_add_hash(sketch_hll_t *hll, uint64_t h)
{
   uint32_t idx;
   uint8_t rank;
   if (!hll)
      return;
   idx = (uint32_t)(h & (SKETCH_HLL_REGISTERS - 1));
   rank = hll_rank(h << SKETCH_HLL_PRECISION);
   if (rank > hll->registers[idx])
      hll->registers[idx] = rank;
   hll->item_count++;
}
