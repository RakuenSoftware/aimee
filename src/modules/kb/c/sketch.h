/* kb_store/sketch.h: KB_STORE persistence for approximate sketch state. */
#pragma once
#include "../headers/sketch.h"
#include <stdint.h>

typedef struct
{
   char file_path[1024];
   char file_hash[32];
   sketch_minhash_t signature;
} kb_store_sketch_minhash_row_t;

/* Load a Bloom filter from KB_STORE for the given scope/feature_family.
 * Returns 0 on success (filter populated), 1 if no row exists (filter zeroed),
 * -1 on error. */
int kb_store_sketch_bloom_load(sketch_bloom_t *out, const char *scope_kind, const char *scope_id,
                               const char *feature_family);

/* Save a Bloom filter to KB_STORE (upsert). Returns 0 on success. */
int kb_store_sketch_bloom_save(const sketch_bloom_t *b, const char *scope_kind,
                               const char *scope_id, const char *feature_family);

int kb_store_sketch_minhash_save(const sketch_minhash_t *sig, const char *scope_kind,
                                 const char *scope_id, const char *feature_family);
int kb_store_sketch_minhash_signature_upsert(const char *project, const char *file_path,
                                             const char *file_hash, const sketch_minhash_t *sig);
int kb_store_sketch_minhash_signature_get(const char *project, const char *file_path,
                                          kb_store_sketch_minhash_row_t *out);
int kb_store_sketch_minhash_signature_delete(const char *project, const char *file_path);
int kb_store_sketch_minhash_signature_delete_project(const char *project);
int kb_store_sketch_minhash_signature_list(const char *project, kb_store_sketch_minhash_row_t *out,
                                           int max_rows);
int kb_store_sketch_minhash_candidate_list(const char *project, const sketch_minhash_t *sig,
                                           kb_store_sketch_minhash_row_t *out, int max_rows);

int kb_store_sketch_count_min_load(sketch_count_min_t *out, const char *scope_kind,
                                   const char *scope_id, const char *feature_family);
int kb_store_sketch_count_min_save(const sketch_count_min_t *cm, const char *scope_kind,
                                   const char *scope_id, const char *feature_family);

int kb_store_sketch_hll_load(sketch_hll_t *out, const char *scope_kind, const char *scope_id,
                             const char *feature_family);
int kb_store_sketch_hll_save(const sketch_hll_t *hll, const char *scope_kind, const char *scope_id,
                             const char *feature_family);
