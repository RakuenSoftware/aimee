/* Descriptor-owned ABI for DB2's deterministic sketch support subset. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SKETCH_BLOOM_M_BITS (1u << 20)
#define SKETCH_BLOOM_K      7
#define SKETCH_BLOOM_BYTES  (SKETCH_BLOOM_M_BITS / 8)

#define SKETCH_MINHASH_PERMUTATIONS 128
#define SKETCH_LSH_BANDS            32
#define SKETCH_LSH_ROWS_PER_BAND    4
#define SKETCH_COUNT_MIN_WIDTH      65536
#define SKETCH_COUNT_MIN_DEPTH      4
#define SKETCH_HLL_PRECISION        12
#define SKETCH_HLL_REGISTERS        (1u << SKETCH_HLL_PRECISION)

typedef struct
{
   uint8_t bits[SKETCH_BLOOM_BYTES];
   uint64_t item_count;
} sketch_bloom_t;

typedef struct
{
   uint64_t values[SKETCH_MINHASH_PERMUTATIONS];
} sketch_minhash_t;

typedef struct
{
   uint32_t counters[SKETCH_COUNT_MIN_DEPTH][SKETCH_COUNT_MIN_WIDTH];
   uint64_t item_count;
} sketch_count_min_t;

typedef struct
{
   uint8_t registers[SKETCH_HLL_REGISTERS];
   uint64_t item_count;
} sketch_hll_t;

void sketch_bloom_init(sketch_bloom_t *b);
void sketch_minhash_init(sketch_minhash_t *sig);
uint64_t sketch_lsh_band_hash(const sketch_minhash_t *sig, int band);
void sketch_count_min_init(sketch_count_min_t *cm);
void sketch_hll_init(sketch_hll_t *hll);
void sketch_hll_add_hash(sketch_hll_t *hll, uint64_t h);
uint64_t sketch_fnv1a(const void *data, size_t len);
