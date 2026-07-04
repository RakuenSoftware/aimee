/* db2/artifacts.h: charter artifact table accessors (DB2).
 *
 * Covers the four charter tables: artifacts, artifact_citations,
 * artifact_links, audit_events.
 * See docs/proposals/done/cross-source-learning-substrate.md */
#ifndef DEC_DB2_ARTIFACTS_H
#define DEC_DB2_ARTIFACTS_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Generate a random UUID string (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    * into buf.  buf must be at least 37 bytes. */
   void db2_artifact_gen_id(char *buf, size_t len);

   /* Insert an artifact row.  id must be a pre-generated UUID from
    * db2_artifact_gen_id.  All string params may be NULL (stored as "").
    * payload_json must be valid JSON or NULL (stored as "{}").
    * Returns 0 on success, -1 on error.  Duplicate id is silently
    * ignored (ON CONFLICT DO NOTHING). */
   int db2_artifact_write(const char *id, const char *kind, const char *state,
                          const char *scope_kind, const char *scope_id, const char *operator_id,
                          double confidence, const char *payload_json);

   /* Extended write: same as db2_artifact_write but accepts explicit attempt_count
    * instead of the implicit 1.  Use for N-attempt synthesis artifacts. */
   int db2_artifact_write_ex(const char *id, const char *kind, const char *state,
                             const char *scope_kind, const char *scope_id, const char *operator_id,
                             double confidence, int attempt_count, const char *payload_json);

   /* Idempotent evidence capture: write a proposed evidence artifact of `kind`
    * (the charter source_kind) with the given scope and a content_hash stored
    * in source_bundle_hash. If an artifact with the same (kind, content_hash)
    * already exists, no row is written and its id is returned instead — so
    * re-uploading identical evidence never duplicates it. Writes the chosen id
    * (new or existing) into id_out (>= 37 bytes; may be NULL).
    * Returns 0 on success, -1 on error. */
   int db2_artifact_write_evidence(const char *kind, const char *scope_kind, const char *scope_id,
                                   const char *operator_id, const char *content_hash,
                                   const char *payload_json, char *id_out, int id_out_len);

   /* Count the distinct cited evidence sources for an artifact (its
    * corroboration). Returns the count (>= 0), or -1 on error. */
   int db2_artifact_citation_count(const char *artifact_id);

   /* Return 1 if a retroactive thumbs-down has been recorded for this
    * (verdict_tag, verdict_scope) — i.e. candidate generation should suppress
    * proposals matching it. Returns 0 if none / on error. */
   int db2_artifact_verdict_suppressed(const char *verdict_tag, const char *verdict_scope);

   /* Read the before_snapshot JSON of an artifact's most recent audit event
    * into out ("" if none). Returns 0 on success, -1 on error. */
   int db2_audit_read_latest_before(const char *artifact_id, char *out, int out_len);

   /* Retroactive thumbs-down rollback: roll the artifact back to restore_state
    * (its pre-commit snapshot state) and write an audit_events row recording the
    * verdict (verdict='thumbs_down') with verdict_tag / verdict_scope /
    * counter_example in their dedicated columns and before/after snapshots.
    * Returns 0 on success, -1 on error. */
   int db2_artifact_review_rollback(const char *artifact_id, const char *restore_state,
                                    const char *verdict_tag, const char *verdict_scope,
                                    const char *counter_example);

   /* Transition an artifact's state field.
    * Returns 0 on success, -1 on error / not found. */
   int db2_artifact_set_state(const char *id, const char *new_state);

   /* Set / read an artifact's target_surface (the promotion target a candidate
    * routes to). Returns 0 on success, -1 on error. The read writes "" when the
    * column is empty. */
   int db2_artifact_set_target_surface(const char *id, const char *target_surface);
   int db2_artifact_target_surface(const char *id, char *out, int out_len);

   /* Register an artifact as a case/guardrail exemplar (insert an
    * exemplar_vectors row; embedding filled asynchronously). Returns 0 on
    * success, -1 on error. */
   int db2_artifact_register_exemplar(const char *artifact_id, const char *collection);

   /* Stamp last_accessed_at for an artifact read path.
    * Returns 0 on success, -1 on error / not found. */
   int db2_artifact_touch(const char *id);

   /* Add a citation from artifact_id → (source_kind, source_id).
    * Returns 0 on success, -1 on error.  Duplicate is silently ignored. */
   int db2_artifact_cite(const char *artifact_id, const char *source_kind, const char *source_id);

   /* Add a directed link between two artifacts.
    * kind is one of: supersedes, supports, contradicts, implements,
    *                 resolved_by, mentions, corrects.
    * Returns 0 on success, -1 on error.  Duplicate is silently ignored. */
   int db2_artifact_link(const char *from_id, const char *to_id, const char *link_kind);

   /* Insert an audit event row.  id and source_artifact_id must be
    * pre-generated UUIDs.  Returns 0 on success, -1 on error. */
   int db2_audit_event_write(const char *id, const char *source_artifact_id,
                             const char *target_surface, const char *target_id,
                             const char *operator_id, const char *scope_kind, const char *scope_id,
                             double applied_confidence, int flagged_for_review,
                             const char *before_json, const char *after_json);

   /* Read-side projection of audit_events for the console action feed. The
    * free-text before/after snapshots are intentionally NOT included (redaction). */
   typedef struct
   {
      char id[64];
      char target_surface[64];
      char target_id[160];
      char operator_id[128];
      char scope_kind[32];
      char scope_id[128];
      char applied_at[32];
      double applied_confidence;
      int flagged_for_review;
   } db2_audit_event_row_t;

   /* List audit events applied within [since, until] (since is REQUIRED — no
    * full-table scans), optionally filtered by scope_kind, most-recent-first.
    * Returns the count written, or -1 (incl. a missing `since`). */
   int db2_audit_event_list(const char *since, const char *until, const char *scope_kind, int limit,
                            db2_audit_event_row_t *out, int max);

   /* Count artifacts matching kind and state (NULL = any).
    * Returns count >= 0 on success, -1 on error. */
   int db2_artifact_count(const char *kind, const char *state);

   /* One proposed artifact row returned by db2_artifact_list_proposed. */
   typedef struct
   {
      char id[37];
      char kind[64];
      char target_surface[64];
      double confidence;
      char created_at[32];
      char payload_json[4096];
      char citation_ids[4096]; /* comma-separated source_ids from artifact_citations */
   } db2_artifact_proposed_t;

   /* List proposed artifacts ordered by confidence DESC.
    * target_surface may be NULL to match all surfaces.
    * Writes up to max_out rows into out.
    * Returns number of rows written, or -1 on error. */
   int db2_artifact_list_proposed(const char *target_surface, int limit,
                                  db2_artifact_proposed_t *out, int max_out);

   /* Reject an artifact: set state = 'rejected' and write an audit_events row
    * with the provided verdict fields.  verdict_tag, verdict_scope, and
    * counter_example may be NULL.
    * Returns 0 on success, -1 on error. */
   int db2_artifact_reject(const char *id, const char *verdict_tag, const char *verdict_scope,
                           const char *counter_example, const char *before_json);

   /* Invalidate artifacts whose citation points at a source span that changed:
    * mark every live (proposed / committed) artifact citing (source_kind,
    * source_id) as state='stale' and write one audit_events row per artifact.
    * A citation matches when it cites the whole source (span 0,0), when the
    * edit is whole-source (edit_span_start == edit_span_end == 0), or when the
    * citation span overlaps [edit_span_start, edit_span_end).
    * Returns the number of artifacts marked stale (>= 0), or -1 on error. */
   /* Version-bump: re-queue every committed curator artifact for re-embed
    * (committed -> proposed). Returns the count. */
   int db2_curator_reembed_all(void);

   int db2_artifact_invalidate_citing(const char *source_kind, const char *source_id,
                                      int edit_span_start, int edit_span_end);

   /* Mark an artifact as reflected (stamp reflected_at = NOW()).
    * Returns 0 on success, -1 on error. */
   int db2_artifact_stamp_reflected(const char *id);

   /* ── Phase 5 retrieval types ─────────────────────────────────────────── */

   /* Full artifact row returned by db2_artifact_read. */
   typedef struct
   {
      char id[37];
      char kind[64];
      char state[32];
      char scope_kind[32];
      char scope_id[128];
      double confidence;
      char payload_json[4096];
      char updated_at[32];
   } db2_artifact_row_t;

   /* One citation entry (artifact_citations table). */
   typedef struct
   {
      char source_kind[64];
      char source_id[256];
   } db2_artifact_citation_t;

   /* One outgoing artifact link (artifact_links table). */
   typedef struct
   {
      char to_id[37];
      char link_kind[64];
   } db2_artifact_link_row_t;

   /* Read a single artifact by UUID.
    * Fills *out with artifact fields; populates citations[0..max_citations-1]
    * and sets *citation_count (may be NULL if caller does not need count).
    * Returns 0 on success, -1 on error or not found. */
   int db2_artifact_read(const char *id, db2_artifact_row_t *out,
                         db2_artifact_citation_t *citations, int max_citations,
                         int *citation_count);

   /* Typed-facet artifact filter for POST /v1/search. Returns live
    * (proposed / committed) artifacts matching ALL supplied facets; any facet
    * that is NULL or "" is a wildcard. `kind` matches the artifact.kind column;
    * `status` and `priority` match the same-named string fields in the
    * narrative payload; `component` matches membership in the payload
    * "components" array (all case-insensitive). When `release_id > 0`, results
    * are bound to that release — only artifacts citing a `kb_document` in the
    * release (via release_docs) are returned; release_id <= 0 means no release
    * binding. The result is precision-exact: every returned row satisfies every
    * supplied facet. Writes up to `max` rows into out; returns the count
    * (>= 0), or -1 on error. */
   int db2_artifact_filter_facets(int64_t release_id, const char *kind, const char *status,
                                  const char *priority, const char *component,
                                  db2_artifact_row_t *out, int max);

   /* Read outgoing links from one artifact.
    * Returns count written into out (>= 0), or -1 on error. */
   int db2_artifact_links_read(const char *id, db2_artifact_link_row_t *out, int max);

   /* Demote artifact to proposed+flagged_for_review.
    * Stamps flagged_for_review=true and flagged_reason=reason in the payload.
    * Returns 0 on success, -1 on error. */
   int db2_artifact_flag_review(const char *id, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ARTIFACTS_H */
