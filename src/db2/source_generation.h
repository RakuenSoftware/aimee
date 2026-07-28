/* source_generation.h: immutable repository snapshots and atomic ref publication. */
#ifndef DEC_DB2_SOURCE_GENERATION_H
#define DEC_DB2_SOURCE_GENERATION_H 1

#include <stdint.h>

typedef struct
{
   int64_t repository_id;
   int64_t snapshot_id;
   int64_t generation_id;
   int64_t project_id;
   char repository_key[512];
   char source_ref[1024];
   char commit_sha[65];
   char tree_sha[65];
   char physical_project[128];
   char state[32];
   char refresh_state[32];
   char source_manifest_hash[65];
   int64_t expected_file_count;
   int64_t indexed_file_count;
   int64_t expected_model_subject_count;
   int64_t model_subject_count;
   int is_default;
   int reused_snapshot;
   int already_current;
} db2_source_generation_t;

typedef struct
{
   int64_t generation_id;
   int64_t repository_id;
   int64_t project_id;
   char repository_key[512];
   char physical_project[128];
   char prior_state[32];
} db2_source_prune_candidate_t;

/* Begin or resume a generation for one exact committed snapshot. A published
 * snapshot is reused immediately; an in-progress snapshot resumes idempotently.
 * No ref pointer is advanced to incomplete data. */
int db2_source_generation_begin(const char *repository_key, const char *source_ref,
                                int is_default, const char *commit_sha, const char *tree_sha,
                                const char *root_label, db2_source_generation_t *out);

/* Validate that all source batches landed in the fresh physical project and
 * move staging -> encoding. expected_file_count is the complete manifest size. */
int db2_source_generation_source_complete(int64_t generation_id,
                                          const char *source_manifest_hash,
                                          int64_t expected_file_count,
                                          db2_source_generation_t *out);

/* Record completion of the model representation pass and move encoding ->
 * validating. actual subjects must cover expected subjects. */
int db2_source_generation_model_complete(int64_t generation_id,
                                         int64_t expected_subject_count,
                                         int64_t actual_subject_count,
                                         const char *model_id,
                                         const char *checkpoint_hash,
                                         const char *tokenizer_hash,
                                         db2_source_generation_t *out);

/* Atomically publish a fully validated generation. Returns -2 if the ref moved
 * to a newer commit while this generation was being built. */
int db2_source_generation_publish(int64_t generation_id, db2_source_generation_t *out);

/* Abort an incomplete generation without changing the current ref pointer. */
int db2_source_generation_abort(int64_t generation_id, const char *reason);

/* Load one generation by opaque id. Returns 1 on hit, 0 on miss, -1 on error. */
int db2_source_generation_get(int64_t generation_id, db2_source_generation_t *out);

/* Attach one parsed code-file row to its exact primary original. The generation
 * must still be staging and the physical project is resolved server-side.
 * Returns 1 when linked, 0 when the path was excluded/not present, -1 on error. */
int db2_source_generation_link_file_evidence(int64_t generation_id, const char *rel_path,
                                             int64_t original_version_id);

/* Record tertiary lineage for current code embeddings, each through its
 * secondary code_file parent to an exact primary version. Returns the number
 * of current file subjects covered, or -1 on error. */
int db2_source_generation_record_embedding_lineage(int64_t generation_id,
                                                    const char *model_id);

/* Resolve repository + explicit/default ref to its current published physical
 * project. Returns 1 on a current hit, 0 when unseen/unpublished, -1 on error. */
int db2_source_ref_resolve_current(const char *repository_key, const char *source_ref,
                                   db2_source_generation_t *out);

/* Retire a merged/deleted ref. It becomes non-retrievable in the same
 * transaction. Its generation is retained while another active ref shares it;
 * otherwise it is scheduled for physical collection after grace_seconds.
 * Pass -1 to use the repository policy. Returns 1 when retired, 0 if the ref
 * was not known, or -1 on error. */
int db2_source_ref_retire(const char *repository_key, const char *source_ref,
                          const char *reason, int grace_seconds);

/* Atomically claim one due, unreferenced physical generation for garbage
 * collection. Returns 1 on a claim, 0 when none are due, -1 on error. */
int db2_source_generation_prune_claim(db2_source_prune_candidate_t *out);

/* Complete a claimed prune after all external stores have been purged. This
 * removes exact primary rows, lineage, the physical project/generation, and an
 * orphaned snapshot. Content-addressed bytes are reclaimed separately only
 * when no remaining original version references them. */
int db2_source_generation_prune_finalize(int64_t generation_id);

/* Return a failed claim to the retry queue without making it retrievable. */
int db2_source_generation_prune_release(int64_t generation_id, const char *reason,
                                        int retry_seconds);

#endif /* DEC_DB2_SOURCE_GENERATION_H */
