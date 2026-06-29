/* kb_payload.h: DB2 domain helper that builds vector payloads for kb chunks.
 *
 * Reads kb_documents rows and assembles the JSON body that the pgvector
 * transport attaches to a vector upsert. Pure DB2 data; no pgvector
 * transport here.
 */
#ifndef DEC_DB2_KB_PAYLOAD_H
#define DEC_DB2_KB_PAYLOAD_H 1

#include <stddef.h>
#include <stdint.h>

#include "../headers/sketch.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Heap-allocated payload JSON for the kb_documents row, or NULL on
    * missing / SQL error.  Caller frees. */
   char *db2_kb_build_document_payload(int64_t doc_id);

   /* (id, file_path, heading_path, line_start, line_end, content) row
    * from kb_documents.  Field sizes mirror the kb_result_t struct
    * the search path consumes. */
   typedef struct
   {
      int64_t id;
      char file_path[1024];
      char file_hash[32];
      char heading_path[256];
      int line_start;
      int line_end;
      char content[8192];
      char doc_kind[16]; /* '' for normal docs, 'pdf' for structured-PDF chunks */
   } db2_kb_document_row_t;

   /* Look up a kb_documents row scoped by (id, project). Returns 1 on
    * hit (out filled), 0 on miss / SQL error / unavailable DB2. */
   int db2_kb_document_fetch(int64_t id, const char *project, db2_kb_document_row_t *out);

   /* Convention-candidate row from kb_documents — tier-friendly subset
    * of fields used by kb_extract_convention_candidates. */
   typedef struct
   {
      char project[64];
      char file_path[1024];
      char heading_path[256];
      char content[8192];
   } db2_kb_convention_row_t;

   /* Pull up to |max| chunks from kb_documents whose file_path matches
    * one of the convention-source patterns (CONTRIBUTING / AGENTS.md /
    * STYLE / CODING / .aimee-rules / .aimee/rules.md / .aimee/context.md
    * / /adr/). Ordered by project, file_path, chunk_index. Returns
    * rows written. */
   int db2_kb_documents_list_convention_candidates(db2_kb_convention_row_t *out, int max);

   /* Look up the stored file_hash for (project, file_path) in
    * kb_documents (any chunk row's column — they all share the same
    * file-level hash). Returns 0 on hit (out filled), -1 on miss /
    * SQL error / unavailable DB2. */
   int db2_kb_documents_get_stored_hash(const char *project, const char *file_path, char *out,
                                        size_t out_len);

   /* Return 1 when DB2 already has evidence or a file-index entry for
    * (project, file_hash), 0 when missing, -1 on SQL / connection error.
    * sample_path is optional and receives one matching path for diagnostics. */
   int db2_kb_documents_hash_exists(const char *project, const char *file_hash, char *sample_path,
                                    size_t sample_path_len);

   /* Build an HLL over distinct file_path values that share (project,
    * file_hash). Returns the number of distinct identifiers added, or
    * -1 on SQL / connection error. */
   int db2_kb_documents_hll_sources_for_hash(const char *project, const char *file_hash,
                                             sketch_hll_t *out);

   /* INSERT OR IGNORE a kb_async_jobs row: (kind, document_id, project,
    * status='pending'). Used by kb_build to enqueue an async embedding
    * job for a freshly-inserted chunk. Returns 0 on success, -1 on
    * SQL / connection error. */
   int db2_kb_async_enqueue(const char *kind, int64_t document_id, const char *project);

   /* Version-bump: re-enqueue extract_doc for every document. Returns count. */
   int db2_curator_reenqueue_extract_all(void);

   /* Invalidation: mark curator artifacts citing a (project,file_path)'s chunks
    * as stale. Call before the chunks are deleted. Returns the count. */
   int db2_curator_invalidate_doc(const char *project, const char *file_path);

   typedef struct
   {
      int64_t id;
      char source_kind[64];
      char source_id[256];
      int artifacts_stale;
      char created_at[32];
   } db2_curator_invalidation_t;

   /* Append an invalidation event (one per invalidated source). */
   void db2_curator_invalidation_record(const char *source_kind, const char *source_id,
                                        int artifacts_stale);
   /* Fetch invalidation events with id > since_id (ascending). Returns count. */
   int db2_curator_invalidations_since(int64_t since_id, db2_curator_invalidation_t *out, int max);

   /* List up to |max| kb_documents.id values that belong to
    * (project, file_path). Caller materializes the ids before issuing
    * follow-up writes (db3 point deletes + the bulk DELETE) because
    * libpq only supports one active result per connection. Returns
    * count written (0 on miss / no DB2). */
   int db2_kb_documents_list_chunk_ids_for_file(const char *project, const char *file_path,
                                                int64_t *out, int max);

   /* DELETE FROM kb_documents WHERE project = ? AND file_path = ?.
    * Best-effort; no return. Caller is responsible for any
    * accompanying db3 point deletes — those run on the materialized
    * id list returned by db2_kb_documents_list_chunk_ids_for_file. */
   void db2_kb_documents_delete_for_file(const char *project, const char *file_path);

   /* INSERT a fresh kb_documents row with DELETE-then-INSERT semantics
    * (callers invoke db2_kb_documents_delete_for_file for the file
    * before inserting any chunks). updated_at is stamped to now.
    * Returns the new document id, or -1 on SQL / connection error. */
   int64_t db2_kb_documents_insert_chunk(const char *project, const char *file_path,
                                         const char *file_hash, int chunk_index,
                                         const char *heading_path, int line_start, int line_end,
                                         const char *content, int token_count);

   /* Set prev_chunk_id on doc_id and next_chunk_id on prev_id, linking
    * a freshly-inserted chunk to its predecessor. No-op when prev_id
    * is non-positive. */
   void db2_kb_documents_link_neighbours(int64_t doc_id, int64_t prev_id);

   /* structured-pdf Phase 1: insert a chunk row also stamping doc_kind='pdf',
    * the caller's chunk_strategy ('heading' or 'page'), and the page_start/
    * page_end span. Returns the new document id, or -1 on error. */
   int64_t db2_kb_documents_insert_chunk_pdf(const char *project, const char *file_path,
                                             const char *file_hash, int chunk_index,
                                             const char *heading_path, int line_start, int line_end,
                                             const char *content, int token_count,
                                             const char *chunk_strategy, int page_start,
                                             int page_end, const char *sensitivity_class,
                                             const char *quarantine_state);

   /* structured-pdf Phase 1: insert one per-line coordinate region for a chunk.
    * bbox must already be normalized to [0,1] (top-left origin, per page).
    * Returns the new region id, or -1 on error. */
   int64_t db2_kb_doc_regions_insert(int64_t chunk_id, const char *document_key, int page_no,
                                     double x0, double y0, double x1, double y1, const char *quote,
                                     int line_index, const char *content_type,
                                     const char *sensitivity_class);

   /* Transaction wrappers (Postgres + sqlite shim). begin/commit return 0 on success,
    * <0 on error; rollback is best-effort. */
   int db2_kb_txn_begin(void);
   int db2_kb_txn_commit(void);
   void db2_kb_txn_rollback(void);

   /* structured-pdf Phase 2/A retrieval. A PDF chunk matched by search_chunks. */
   typedef struct
   {
      int64_t chunk_id;
      char document_key[1024];
      char content[8192];
      int page_start;
      int page_end;
      char sensitivity_class[16];
      double score;       /* Phase A2: relevance — vector cosine for a vector hit, else a fixed
                           * strong-lexical constant for a substring hit. */
      int matched_vector; /* Phase A2: 1 if this candidate came from the PDF-vector leg. */
      int has_citation;   /* Phase A2: 1 if the chunk has >=1 kb_doc_regions citation. */
   } db2_kb_pdf_chunk_t;

   /* Phase A3: per-query-over-corpus answerability — "given THIS query, how well can
    * the KB answer it." A KB-side SHARED judgment, deliberately kept distinct from the
    * server's per-user confidence tier. Combiner is a documented deterministic function
    * of query-scoped (top_score, coverage, saturation) and corpus-scoped (table_facts)
    * inputs; see db2_kb_pdf_search_chunks for the weights. */
   typedef struct
   {
      double score;     /* [0,1] */
      char label[8];    /* "NONE" | "LOW" | "MEDIUM" | "HIGH" */
      double top_score;  /* query-scoped: best candidate relevance among the top-k hits */
      double coverage;   /* query-scoped: fraction of query terms present across matched chunks */
      double saturation; /* query-scoped: hit-count saturation = min(1, n_hits / target_k) */
      int table_facts;   /* corpus-scoped: table-cell facts for query entities (§B; 0 until built) */
   } db2_kb_answerability_t;

   /* Phase A2 two-stage retrieval: lexical (case-insensitive substring) AND — when the
    * kb_pdf_vector capability is on and an embedder is available — a vector candidate leg
    * over the ISOLATED kb_pdf_embeddings relation, merged + deduped by chunk_id. ALWAYS
    * excludes doc_kind != 'pdf' and quarantine_state='pending' (restricted/pending docs are
    * withheld) on BOTH legs. Degrades to lexical-only when the embedder/capability is
    * absent. `ans_out` (may be NULL) receives the Phase A3 answerability judgment for the
    * query. Returns the number of chunks written (<= max). */
   int db2_kb_pdf_search_chunks(const char *project, const char *query, int max,
                                db2_kb_pdf_chunk_t *out, db2_kb_answerability_t *ans_out);

   /* One coordinate region (a PDF line) for a citation. */
   typedef struct
   {
      int page_no;
      double x0, y0, x1, y1; /* normalized [0,1], top-left origin */
      char quote[1024];
      int line_index;
      char content_type[16];
   } db2_kb_pdf_region_t;

   /* Fetch a chunk's regions ordered by line_index (the line-level citations). Returns the
    * number written (<= max). */
   int db2_kb_doc_regions_for_chunk(int64_t chunk_id, db2_kb_pdf_region_t *out, int max);

   /* §6 quarantine admin (owner action). For a (project, document_key) structured PDF
    * currently in quarantine_state='pending': confirm clears the state (the doc becomes
    * retrievable via search_chunks); reject purges its chunks + regions. Both return the
    * number of pending chunks acted on (>0), 0 if there is no pending PDF for that key, or
    * -1 on error. */
   int db2_kb_pdf_quarantine_confirm(const char *project, const char *document_key);
   int db2_kb_pdf_quarantine_reject(const char *project, const char *document_key);

   /* Phase A1: re-enqueue an embed_pdf job for every retrievable (non-pending) PDF
    * chunk so the isolated kb_pdf_embeddings relation is re-derived (used by the
    * dim-change reset, which truncates it). No-op when kb_pdf_vector_enabled is
    * off. Returns the number of jobs enqueued. */
   int db2_kb_pdf_reembed_all(void);

   /* Count rows in kb_async_jobs for a given kind (e.g. "embed_pdf"). Test/observability
    * helper. Returns the count (>=0) or -1 on error. */
   int db2_kb_async_count_kind(const char *kind);

   /* structured-pdf Phase B: a recognised table cell. Stored ONLY in kb_table_cells (never
    * in the shared typed_facts table — see schema.sql). */
   typedef struct
   {
      int64_t id;
      int64_t region_id;
      int page_no;
      int cell_row;
      int cell_col;
      char cell_text[2048];
      char subject[256];
      char relation[128];
      char object[512];
      int tsr_confidence; /* 0-100 */
      char sensitivity_class[16];
   } db2_kb_table_cell_t;

   /* Insert one table cell linked to its source kb_doc_regions row. The denormalised
    * document_key/sensitivity_class come from the region (the live ACL is still the
    * authority at read time). Returns the new id (>0) or -1. */
   int db2_kb_table_cell_insert(int64_t region_id, const char *document_key, int page_no,
                                int cell_row, int cell_col, const char *cell_text,
                                const char *subject, const char *relation, const char *object,
                                int tsr_confidence, const char *sensitivity_class);

   /* lookup_table: structured cells for a (project, document_key[, page_no]) gated by the FULL
    * PDF ACL — doc_kind='pdf' AND quarantine_state<>'pending' AND project — applied via a join
    * to the authoritative kb_documents row (a guessed/foreign document_key returns empty).
    * page_no < 0 returns all pages. Returns the number written (<= max). */
   int db2_kb_table_cells_lookup(const char *project, const char *document_key, int page_no,
                                 db2_kb_table_cell_t *out, int max);

   /* Set/get the per-document TSR outcome (drives lookup_table's tsr_status marker).
    * set: scoped to the (project, file_path) PDF chunks. get: returns the state for a
    * READABLE doc (same ACL as lookup), writing "" + returning 0 if the doc is absent/
    * withheld/unreadable, 1 on a hit. */
   void db2_kb_documents_set_tsr_state(const char *project, const char *file_path,
                                       const char *state);
   int db2_kb_pdf_tsr_state(const char *project, const char *document_key, char *out, size_t out_len);

   /* structured-pdf Phase C: a visual asset (crop) row. blob_ref (the sha256) is DELIBERATELY
    * absent from this struct — it is KB-internal and never surfaced to a client; the agent-
    * facing list returns only the opaque id + metadata. */
   typedef struct
   {
      int64_t id;
      int page_no;
      double x0, y0, x1, y1;
      char kind[32];
      char caption[512];
      char content_type[48];
      char sensitivity_class[16];
   } db2_kb_doc_asset_t;

   /* Insert a crop asset row. blob_ref is the content-addressed sha256 (KB-internal). The
    * denormalised document_key/sensitivity_class come from the source doc (the live ACL is the
    * authority at read time). Returns the new opaque id (>0) or -1. */
   int db2_kb_doc_asset_insert(const char *document_key, int page_no, double x0, double y0,
                               double x1, double y1, const char *kind, const char *caption,
                               const char *content_type, const char *blob_ref,
                               const char *sensitivity_class);

   /* open_asset resolve: given the OPAQUE row id, return its blob_ref + content_type IFF the
    * caller may read the asset's document — gated by a join to the AUTHORITATIVE kb_documents
    * row (doc_kind='pdf' AND quarantine_state<>'pending' AND project). A guessed/foreign/
    * withheld id returns 0 (and empty out). Returns 1 on a readable hit. The sha256 stays
    * inside this call — the route streams bytes from the blob store and never echoes it. */
   int db2_kb_doc_asset_open(const char *project, int64_t asset_id, char *blob_ref_out,
                             size_t ref_cap, char *content_type_out, size_t ct_cap);

   /* List a document's assets (metadata + opaque id, NO blob_ref) gated by the full PDF ACL —
    * so an agent can discover ids to open_asset. Bound to the authoritative kb_documents
    * file_path. Returns the number written (<= max). */
   int db2_kb_doc_assets_list(const char *project, const char *document_key,
                              db2_kb_doc_asset_t *out, int max);

   /* Delete all asset rows for a document (re-ingest / purge). The blobs are reclaimed
    * separately by the reconciliation sweep (refcount-by-scan). Returns rows deleted, -1 err. */
   int db2_kb_doc_assets_delete_for_doc(const char *project, const char *document_key);

   /* Reconciliation refcount: 1 if ANY kb_doc_assets row references blob_ref, else 0 (-1 err).
    * A blob with no referrer is an orphan and may be unlinked. */
   int db2_kb_blob_ref_referenced(const char *blob_ref);

   /* §5 evidence escalation reads. All withhold quarantine_state='pending' (restricted)
    * documents. Return the number written (<= max). */

   /* open_page: every coordinate region on (project, document_key, page_no), ordered by
    * line_index — the full set of citations for a page. */
   int db2_kb_pdf_open_page(const char *project, const char *document_key, int page_no,
                            db2_kb_pdf_region_t *out, int max);

   /* open_neighbors: the prev/next reading-order chunks of `chunk_id`, scoped to `project`
    * (the project predicate prevents cross-scope chunk-id enumeration). 0-2 results. */
   int db2_kb_pdf_open_neighbors(const char *project, int64_t chunk_id, db2_kb_pdf_chunk_t *out,
                                 int max);

   /* inspect_structure: a document's chunk outline (chunk_index + page span + heading_path),
    * ordered by chunk_index. */
   typedef struct
   {
      int chunk_index;
      int page_start;
      int page_end;
      char heading_path[256];
   } db2_kb_pdf_outline_t;
   int db2_kb_pdf_inspect_structure(const char *project, const char *document_key,
                                    db2_kb_pdf_outline_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KB_PAYLOAD_H */
