/* entity_registry.h: surrogate-id entity canonicalization (typed-fact §3 / P2).
 *
 * Gives each entity a globally-unique surrogate `canonical_id` + a kind, and maps
 * display names -> canonical id via single-hop aliases (UNIQUE name_norm, so
 * "DevBox" / "the workstation" can resolve to one id instead of fragmenting).
 * This phase (P2a) implements the synchronous hot path: explicit binding +
 * normalized exact-match resolution + get-or-create + alias listing + a status
 * lifecycle (active/provisional/merged) with merged_into followed on resolve.
 * Conservative implicit near-match, the ambiguity conflict queue, and
 * merge/unmerge are P2b. See typed-fact §3. */
#ifndef DEC_DB2_ENTITY_REGISTRY_H
#define DEC_DB2_ENTITY_REGISTRY_H 1

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* entity_registry.status values. */
#define ENTITY_STATUS_ACTIVE      "active"
#define ENTITY_STATUS_PROVISIONAL "provisional"
#define ENTITY_STATUS_MERGED      "merged"

   /* Normalize a display name for matching: lower-cased, leading/trailing
    * whitespace trimmed, internal whitespace runs collapsed to one space.
    * Punctuation is preserved (so "192.168.1.254" stays intact). NUL-terminated. */
   void entity_name_normalize(const char *in, char *out, size_t out_len);

   /* Create a fresh registry row of `kind` and `status` (use ENTITY_STATUS_*),
    * returning its surrogate canonical_id (>0) or -1 on error. */
   int64_t db2_entity_register(int kind, const char *status);

   /* Bind a display name to a canonical id (single-hop alias). The first binding
    * for a name wins (ON CONFLICT(name_norm) DO NOTHING) — never silently
    * re-points an existing name. `is_preferred` marks the entity's display name.
    * Returns 0 on success (incl. already-bound), -1 on error. */
   int db2_entity_alias_bind(const char *name, int64_t canonical_id, int is_preferred);

   /* Resolve a display name to its canonical id, following a merged row's
    * merged_into pointer one hop. Returns the canonical id (>0), or 0 if the name
    * is unknown / suppressed, or -1 on error. */
   int64_t db2_entity_resolve(const char *name);

   /* Get-or-create: resolve `name`; if unknown, register a new entity of `kind`
    * and bind `name` as its preferred alias. The synchronous explicit-binding hot
    * path. Returns the canonical id (>0) or -1. */
   int64_t db2_entity_register_named(const char *name, int kind);

   /* The entity's kind, or -1 if canonical_id is unknown. */
   int db2_entity_kind(int64_t canonical_id);

   /* Mark `from_id` as merged into `into_id`: sets status='merged' + merged_into so
    * db2_entity_resolve transparently follows it (one hop). A P2 primitive that
    * full merge/unmerge (P2b) builds on. 0 on success, -1 on error/bad args. */
   int db2_entity_mark_merged(int64_t from_id, int64_t into_id);

   /* List the (non-suppressed) display names bound to `canonical_id` into out
    * (each truncated to 128 bytes incl. NUL). Returns the count written (>=0), or
    * -1 on error (bad args / DB failure). */
   int db2_entity_aliases_for(int64_t canonical_id, char (*out)[128], int max);

   /* First-class merge: collapse `from_id` into `into_id` (mark merged + record an
    * audit row so it is reversible). Reversible without moving aliases — resolve
    * follows the merged_into pointer (single hop). Returns the merge audit id (>0)
    * or -1. */
   int64_t db2_entity_merge(int64_t from_id, int64_t into_id);

   /* Reverse a recorded merge: restore the merged entity to active and mark the
    * audit row undone. 0 on success, -1 on error / unknown / already-undone. */
   int db2_entity_unmerge(int64_t merge_id);

   /* entity_name_conflicts: the true-ambiguity queue (§3 tier 3). Record an open
    * conflict for a name (idempotent on name_norm; bumps priority on repeat).
    * Returns the row id (>0) or -1. */
   int64_t db2_entity_conflict_record(const char *name);

   /* Set a conflict's status (use one of the literals below). 0/-1. */
#define ENTITY_CONFLICT_OPEN     "open"
#define ENTITY_CONFLICT_RESOLVED "resolved"
#define ENTITY_CONFLICT_FAILED   "failed"
   int db2_entity_conflict_set_status(int64_t conflict_id, const char *status);

   /* Count conflicts in a given status (NULL = all). >=0, or -1 on error. */
   int db2_entity_conflict_count(const char *status);

   /* The priority counter for a name's conflict row (>=0), or -1 if absent /
    * error. Lets callers observe the bump-on-repeat behavior. */
   int db2_entity_conflict_priority(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ENTITY_REGISTRY_H */
