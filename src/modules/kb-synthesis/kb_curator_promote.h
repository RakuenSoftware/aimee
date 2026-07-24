/* kb_curator_promote.h: deep-curator promote_entity pass.
 *
 * An entity whose evidence spans many distinct sources is a broader concept
 * than its (narrow) origin scope implies. promote_entity detects such entities
 * and promotes them one step up the scope lattice (project -> workspace ->
 * global): it writes a new entity at the broader scope, links old -> new via
 * `supersedes`, carries the old entity's `mentions` onto the new one, records
 * an audit_events row, and marks the old entity `superseded` so it is not
 * re-promoted.
 *
 * Off by default (kb.curator.promote_entity.enabled). No DB1 access. */
#ifndef KB_CURATOR_PROMOTE_H
#define KB_CURATOR_PROMOTE_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Process one promotion candidate. Returns 1 if an entity was promoted, 0 if
 * none was eligible (or no DB / pass disabled), so the drain can rate-limit. */
#include <stddef.h>

/* Test seam: pick the highest-evidence promotion candidate (a committed,
 * non-global, not-yet-superseded entity cited by >= min_sources distinct
 * `mentions` sources). Returns 1 and fills the out params on a hit, else 0.
 * *payload_out is heap-allocated (caller frees) on a hit. */
int kb_curator_promote_pick(void *conn, int min_sources, char *id, size_t id_len,
                            char **payload_out, char *scope_kind, size_t sk_len, char *scope_id,
                            size_t si_len);

int kb_curator_promote_entity_one(const kb_curator_extract_opts_t *opts);

/* Pure scope-lattice step: the next broader scope_kind for `kind`, or NULL if
 * `kind` is already the top ("global") or unrecognized. Exposed for tests. */
const char *kb_curator_scope_next_kind(const char *kind);

/* Resolve the broader scope for (kind, id): project -> its owning workspace
 * (from the projects table) when known, else straight to global; workspace ->
 * global. Writes out_kind/out_id; returns 1 if a broader scope exists, 0 if
 * already global / unknown. Shared with resolve_entities for cross-scope
 * entity resolution (a narrow mention resolving onto a broader canonical). */
int kb_curator_broaden_scope(void *conn, const char *kind, const char *id, char *out_kind,
                             size_t kn, char *out_id, size_t in);

#endif /* KB_CURATOR_PROMOTE_H */
