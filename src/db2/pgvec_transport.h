#ifndef DEC_DB2_PGVEC_TRANSPORT_H
#define DEC_DB2_PGVEC_TRANSPORT_H 1

#include <stdint.h>

/* Low-level pgvector transport — internal to db2.
 * All operations use the shared db2 postgres connection (db2_conn()). */

/* Table name constants. */
#define PGVEC_MEMORY_TABLE            "memory_embeddings"
#define PGVEC_KB_TABLE                "kb_embeddings"
#define PGVEC_CODE_TABLE              "code_embeddings"
#define PGVEC_CURATOR_ENTITY_TABLE    "curator_entity_vectors"
#define PGVEC_CURATOR_NARRATIVE_TABLE "curator_narrative_vectors"
#define PGVEC_CURATOR_CLAIM_TABLE     "curator_claim_vectors"
#define PGVEC_CURATOR_CODE_UNIT_TABLE "curator_code_unit_vectors"

/* Check whether the embedding table has the HNSW index (i.e. is "ready").
 * Returns 1 if ready, 0 if table exists but no HNSW index, -1 on error. */
int pgvec_table_ready(const char *table);

/* Return the number of rows in the embedding table. Returns -1 on error. */
int64_t pgvec_point_count(const char *table);

/* Create (or re-create) the HNSW index on an embedding table.
 * recreate=1 truncates the table and drops the old index first. */
int pgvec_ensure_index(const char *table, int dim, int recreate);

/* Returns 1 if the pgvectorscale extension is installed in the connected database, 0 otherwise. */
int pgvec_vectorscale_available(void);

/* Resolve corpus index type from config policy.
 * configured: "auto"|"hnsw"|"diskann".  Returns "hnsw" or "diskann".
 * "diskann" is only returned when vectorscale_available=1 AND (configured="diskann" OR
 * configured="auto" with corpus_rows >= diskann_threshold). */
const char *pgvec_corpus_index_type(const char *configured, int64_t corpus_rows,
                                    int vectorscale_available, int64_t diskann_threshold);

/* Create (or re-create) a corpus vector index of the given type ("hnsw" or "diskann").
 * Falls back to HNSW with a LOG_WARN if diskann is requested but vectorscale is absent.
 * recreate=1 drops existing corpus indexes first (no table truncate).
 * Returns 0 on success, -1 on error. */
int pgvec_ensure_corpus_index(const char *table, const char *index_type, int recreate);

/* Upsert a single memory embedding row.  Extracts record_type, primary_scope,
 * workspace, project, kind from payload_json. */
int pgvec_memory_upsert(int64_t point_id, const float *vec, int dim, const char *payload_json);

/* Delete a single memory embedding row by point_id. */
int pgvec_memory_delete(int64_t point_id);

/* Deep tier: write a 2560-dim deep embedding to the halfvec column embedding_deep
 * of an existing memory_embeddings row (leaves the live `embedding` untouched). */
int pgvec_memory_deep_update(int64_t point_id, const float *vec, int dim);

/* Deep tier: collect up to `max` memory point_ids still missing a deep embedding
 * (oldest first). Returns the count written to `ids`, or -1 on error. */
int pgvec_memory_deep_candidates(int64_t *ids, int max);

/* Deep tier: nearest-neighbour search over the halfvec embedding_deep index. */
int pgvec_memory_deep_search(const float *vec, int dim, int limit, int64_t *ids, double *scores,
                             int max);

/* Upsert a single kb embedding row.  Extracts project from payload_json. */
int pgvec_kb_upsert(int64_t point_id, const float *vec, int dim, const char *payload_json);

/* Upsert a batch of kb embedding rows. */
int pgvec_kb_upsert_batch(const int64_t *ids, const float *vecs, int dim,
                          const char *const *payloads, int count);

/* Delete a single kb embedding row by point_id. */
int pgvec_kb_delete(int64_t point_id);

/* Delete all kb embedding rows for a given project. */
int pgvec_kb_delete_project(const char *project);

/* Paginate point_ids from an embedding table in ascending point_id order.
 * Set offset=-1 to start from the beginning.
 * Returns the number of ids written, -1 on error.
 * Sets *done=1 when no more pages remain, *next_offset to the start of the
 * next page (only valid when done=0). */
int pgvec_scroll(const char *table, int64_t offset, int64_t *ids_out, int max,
                 int64_t *next_offset_out, int *done_out);

/* Search memory_embeddings by cosine distance.
 * record_type: required equality filter.
 * kinds/n_kinds: optional IN filter on the kind column (NULL/0 = no filter).
 * workspace/project: optional scope OR filter; pass "" to skip.
 * Returns number of results written (<= max), -1 on error. */
int pgvec_memory_search(const float *vec, int dim, const char *record_type,
                        const char *const *kinds, int n_kinds, const char *workspace,
                        const char *project, int limit, int64_t *ids, double *scores, int max);

/* Search kb_embeddings by cosine distance, filtered to a project.
 * Returns number of results written (<= max), -1 on error. */
int pgvec_kb_search(const char *project, const float *vec, int dim, int limit, int64_t *ids,
                    double *scores, int max);

/* Upsert a single curator entity vector row.  Stores scope_kind, scope_id,
 * canonical_name and artifact_id as first-class columns (not embedded in
 * payload_json) so the resolve_entities pass can filter candidates by scope at
 * search time and map a matched point_id back to its source entity artifact. */
int pgvec_curator_entity_upsert(int64_t point_id, const float *vec, int dim, const char *scope_kind,
                                const char *scope_id, const char *canonical_name,
                                const char *artifact_id, const char *payload_json);

/* Reverse-map a curator_entity_vectors point_id to its source entity.  Writes
 * the stored artifact_id and canonical_name into the caller buffers (either may
 * be NULL to skip).  Returns 1 on a hit, 0 if no such point_id, -1 on error.
 * Used by the resolve_entities judge band and fuzzy contradiction mining to
 * turn a nearest-neighbour point_id back into the artifact it represents. */
int pgvec_curator_entity_lookup(int64_t point_id, char *artifact_id_out, int aid_len,
                                char *name_out, int name_len);

/* Delete a single curator_entity_vectors row by point_id. */
int pgvec_curator_entity_delete(int64_t point_id);

/* Search curator_entity_vectors by cosine distance.  scope_kind/scope_id are
 * optional equality filters; pass NULL or "" to skip either.  Returns the
 * number of results written (<= max), -1 on error. */
int pgvec_curator_entity_search(const char *scope_kind, const char *scope_id, const float *vec,
                                int dim, int limit, int64_t *ids, double *scores, int max);

/* Upsert a single curator narrative vector row.  Stores artifact_id, kind,
 * doc_id, status, priority columns so the deep-curator filter-first doc
 * retrieval can narrow candidates at search time. */
int pgvec_curator_narrative_upsert(int64_t point_id, const float *vec, int dim,
                                   const char *artifact_id, const char *kind, const char *doc_id,
                                   const char *status, const char *priority,
                                   const char *payload_json);

/* Delete a single curator_narrative_vectors row by point_id. */
int pgvec_curator_narrative_delete(int64_t point_id);

/* Search curator_narrative_vectors by cosine distance.  kind/status/priority
 * are optional equality filters; pass NULL or "" to skip any.  Returns the
 * number of results written (<= max), -1 on error. */
int pgvec_curator_narrative_search(const char *kind, const char *status, const char *priority,
                                   const float *vec, int dim, int limit, int64_t *ids,
                                   double *scores, int max);

/* Upsert a single curator claim vector row with TWO named vector columns
 * (subj_attr_vec and value_vec).  artifact_id, subject, attribute, value and
 * claim_kind are stored as first-class columns so detect_contradictions can
 * filter and rank on either vector at search time. */
int pgvec_curator_claim_upsert(int64_t point_id, const float *subj_attr_vec, const float *value_vec,
                               int dim, const char *artifact_id, const char *subject,
                               const char *attribute, const char *value, const char *claim_kind,
                               const char *payload_json);

/* Delete a single curator_claim_vectors row by point_id. */
int pgvec_curator_claim_delete(int64_t point_id);

/* Search curator_claim_vectors by cosine distance.  which_vec selects the
 * named vector column to rank on: "subj_attr" or "value" (any other value is
 * rejected, returns -1).  claim_kind is an optional equality filter; pass
 * NULL or "" to skip.  Returns the number of results written (<= max), -1
 * on error. */
int pgvec_curator_claim_search(const char *which_vec, const char *claim_kind, const float *vec,
                               int dim, int limit, int64_t *ids, double *scores, int max);

/* Upsert a single curator code unit vector row with THREE named vector columns
 * (intent_vec, signature_vec, body_vec).  artifact_id, file_path, def_kind,
 * signature and body_hash are stored as first-class columns so the deep-curator
 * per-definition semantic code store can filter and rank on any of the three
 * vectors at search time. */
int pgvec_curator_code_unit_upsert(int64_t point_id, const float *intent_vec,
                                   const float *signature_vec, const float *body_vec, int dim,
                                   const char *artifact_id, const char *file_path,
                                   const char *def_kind, const char *signature,
                                   const char *body_hash, const char *payload_json);

/* Delete a single curator_code_unit_vectors row by point_id. */
int pgvec_curator_code_unit_delete(int64_t point_id);

/* Search curator_code_unit_vectors by cosine distance.  which_vec selects the
 * named vector column to rank on: "intent", "signature" or "body" (any other
 * value, including NULL, returns -1).  def_kind is an optional equality
 * filter; pass NULL or "" to skip.  Returns the number of results written
 * (<= max), -1 on error. */
int pgvec_curator_code_unit_search(const char *which_vec, const char *def_kind, const float *vec,
                                   int dim, int limit, int64_t *ids, double *scores, int max);

/* Cumulative search latency counters (thread-safe).
 * reset=1 resets all counters after reading. */
void pgvec_search_latency_snapshot(int64_t *total_us, int64_t *count, int64_t *max_us, int reset);

/* Code vector operations (Phase 5). */
int pgvec_code_upsert(int64_t point_id, const float *vec, int dim, const char *project,
                      const char *node_key, const char *file_path, const char *symbol,
                      const char *content_hash, const char *body_hash, const char *payload_json);
int pgvec_code_delete(int64_t point_id);
int pgvec_code_delete_project(const char *project);
int pgvec_code_search(const char *project, const float *vec, int dim, int limit, int64_t *ids,
                      double *scores, int max);
/* Returns 1 if a code_embeddings row with (project, node_key, content_hash,
 * body_hash) exists. body_hash may be blank for legacy callers. */
int pgvec_code_exists_by_hash(const char *project, const char *node_key, const char *content_hash,
                              const char *body_hash);

#endif /* DEC_DB2_PGVEC_TRANSPORT_H */
