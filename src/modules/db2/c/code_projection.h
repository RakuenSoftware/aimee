/* src/modules/db2/c/code_projection.h: code-index graph projection ledger — Postgres. */

#ifndef CODE_PROJECTION_H
#define CODE_PROJECTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Projection generation state machine:
    *   pending -> visible  (publish)
    *   pending -> aborted  (abort on error)
    *   visible -> superseded (on next publish)
    * At most one 'visible' generation per project at any time. */
   typedef enum
   {
      CPG_PENDING = 0,
      CPG_VISIBLE = 1,
      CPG_SUPERSEDED = 2,
      CPG_ABORTED = 3
   } code_projection_state_t;

   /* --- Generation lifecycle --- */

   /* Create a new pending generation for project.  Returns the new id or
    * -1 on error. */
   int64_t db2_code_projection_generation_create(const char *project);

   /* Publish: atomically flip gen_id from pending -> visible and any prior
    * visible generation -> superseded.  Updates edge projection_generation_id
    * for all code_projection_edges in gen_id.  Returns 0 on success, -1 on
    * error. */
   int db2_code_projection_generation_publish(int64_t gen_id, const char *project);

   /* Abort a pending generation (sets state='aborted', stamps aborted_at).
    * Returns 0 on success, -1 on error. */
   int db2_code_projection_generation_abort(int64_t gen_id, const char *error_msg);

   /* Return the currently visible generation id for project, or 0 if none.
    * Returns -1 on DB error. */
   int64_t db2_code_projection_visible_id(const char *project);

   /* Content fingerprint (hex) of a project's code: md5 over (path, file-hash) of
    * all files. Identical contents -> identical fingerprint, so the drain can skip
    * an unchanged project. Returns 0 on success, -1 on error. */
   int db2_code_projection_project_fingerprint(const char *project, char *out, size_t out_len);

   /* Record the content fingerprint that produced this generation (stored in
    * code_projection_generations.source_hash). Returns 0 on success, -1 on error. */
   int db2_code_projection_generation_set_source_hash(int64_t gen_id, const char *source_hash);

   /* Fingerprint stored on the project's currently-visible generation, into out
    * (empty when there is no visible generation). Returns 0 on success, -1 on error. */
   int db2_code_projection_visible_source_hash(const char *project, char *out, size_t out_len);

   /* Update edge/node counts on a generation.  Returns 0 on success. */
   int db2_code_projection_generation_update_counts(int64_t gen_id, int64_t edge_count,
                                                    int64_t node_count);

   /* Delete superseded/aborted generations older than min_days_old.
    * CASCADE deletes their code_projection_edges rows.  Returns deleted count. */
   int db2_code_projection_cleanup_old(const char *project, int min_days_old);

   /* --- Edge ledger --- */

   /* Record a projected edge in the bookkeeping table.  Returns 0 on success,
    * -1 on error. */
   int db2_code_projection_edge_record(int64_t gen_id, const char *project, const char *source,
                                       const char *relation, const char *target,
                                       const char *source_hash);

   /* Upsert a code-projection edge into entity_edges, preserving observed weight,
    * utility_score, and utility_touched_at for edges that already exist.
    * Sets structural_weight, structural_updated_at, edge_origin='code_projection',
    * and projection_generation_id.  Returns 0 on success, -1 on error. */
   int db2_code_projection_edge_upsert(int64_t gen_id, const char *project, const char *source,
                                       const char *relation, const char *target, int relation_id,
                                       int subject_kind, int object_kind, int structural_weight);

   /* One edge of a project's visible projection graph (for analytics / read-out).
    * structural_weight is derived from the relation's structural trust. */
   typedef struct
   {
      char source[512]; /* kept in sync with KB_GRAPH_NODE_MAX so analytics doesn't */
      char relation[64];
      char target[512]; /* collapse two long node names under a truncated prefix    */
      int structural_weight;
   } code_projection_edge_t;

   /* List the edges of project's currently-visible generation into out[] (up to
    * max). Returns the count written (0 if no visible generation), or -1 on error.
    * Used by graph analytics (§4 hub/centrality) — a read-only projection. */
   int db2_code_projection_list_edges(const char *project, code_projection_edge_t *out, int max);

   /* List the edges of a SPECIFIC generation (any state) into out[] (up to max),
    * ordered source,target. Returns the count written (0 if the generation has no
    * edges), or -1 on error. Used to compute community membership for the
    * just-published generation regardless of visible state. */
   int db2_code_projection_list_edges_for_gen(int64_t gen_id, code_projection_edge_t *out, int max);

   /* --- Community membership (graph-feedback S-community) --- */

   /* One node's community assignment within a generation. */
   typedef struct
   {
      char node_id[512];      /* graph node id (edge endpoint)              */
      char community_id[512]; /* min-member node id (lex), generation-local */
   } code_projection_community_t;

   /* Replace the community membership for gen_id: delete any existing rows for the
    * generation, then insert rows[0..n). Idempotent for a given (gen, partition).
    * Returns 0 on success, -1 on error. */
   int db2_code_projection_communities_replace(int64_t gen_id, const char *project,
                                               const code_projection_community_t *rows, int n);

   /* List the community rows of gen_id into out[] (up to max), ordered by node_id.
    * Returns the count written (0 if none), or -1 on error. */
   int db2_code_projection_communities_list(int64_t gen_id, code_projection_community_t *out,
                                            int max);

   /* --- Generation metadata (graph-feedback S2 snapshot diff) --- */

   typedef struct
   {
      int64_t id;
      char project[256];
      char state[16];
      char source_hash[128];
      char extractor_version[64];
      char pipeline_version[64];
   } code_projection_generation_meta_t;

   /* Load a generation's metadata into *out. Returns 0 if found, 1 if no such
    * generation, -1 on error. */
   int db2_code_projection_generation_meta(int64_t gen_id, code_projection_generation_meta_t *out);

   /* One row of the generation list (for a diff route's 409 "available
    * generations" response). */
   typedef struct
   {
      int64_t id;
      char state[16];
      char started_at[32];
   } code_projection_generation_row_t;

   /* List a project's generations, newest first, into out[] (up to max). Returns
    * the count written, or -1 on error. */
   int db2_code_projection_generations_list(const char *project,
                                            code_projection_generation_row_t *out, int max);

   /* --- Full project sync --- */

   /* Sync all code-index facts for project into entity_edges under gen_id.
    * Reads projects/files/terms/file_exports/file_imports/code_calls from DB2
    * and upserts typed edges.  Returns edge count on success, -1 on error. */
   int64_t db2_code_projection_sync_project(const char *project, int64_t gen_id);

#ifdef __cplusplus
}
#endif

#endif /* CODE_PROJECTION_H */
