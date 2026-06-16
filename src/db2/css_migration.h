/* db2/css_migration.h: CSS migration pipeline driver (WP-F).
 *
 * The "enumerate-and-track driver" the plan calls the only new code for #5: it
 * enumerates conversion units (components) from the WP-D component join, tracks
 * each unit's state through the pipeline, and records the WP-E oracle verdict +
 * WP-D resolver coverage that gate auto-accept vs. human/roundtable review. The
 * per-unit conversion itself reuses the existing delegate machinery
 * (delegate_run_inline / delegate_ensemble) and runs each unit in its own git
 * worktree (no shared-tree fan-out); that orchestration is invoked with these
 * primitives and is the user-gated pilot (one component end-to-end first).
 *
 * Also provides the degraded #2 convention model: a rules-document generator
 * that derives a MIGRATION.md-style spec from an exemplar project's style graph
 * (no typed-fact dependency).
 */
#ifndef DEC_DB2_CSS_MIGRATION_H
#define DEC_DB2_CSS_MIGRATION_H 1

#include "../headers/aimee.h" /* MAX_PATH_LEN */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CSS_MIGRATION_PROJECT_MAX 256
#define CSS_MIGRATION_STATE_MAX   32

   typedef struct
   {
      char unit_path[MAX_PATH_LEN];
      char state[CSS_MIGRATION_STATE_MAX]; /* pending|converting|verified|needs_review|failed */
      int total_tokens;
      int resolved_tokens;
      int oracle_equivalent; /* -1 unknown, 0 no, 1 yes */
      char note[256];
   } css_migration_unit_t;

   typedef enum
   {
      CSS_MIGRATION_GATE_AUTO_ACCEPT = 0, /* oracle-equivalent AND coverage >= threshold */
      CSS_MIGRATION_GATE_NEEDS_REVIEW = 1 /* route to human / roundtable */
   } css_migration_gate_t;

   /* Coverage gate (pure): a conversion may auto-accept ONLY when the oracle
    * proved equivalence AND the resolver coverage meets the threshold; otherwise
    * it must be reviewed (the WP-E coverage gate — incomplete resolver inputs
    * make oracle equivalence unsound). coverage_threshold_pct in [0,100]. */
   css_migration_gate_t css_migration_gate(int resolved_tokens, int total_tokens,
                                           int oracle_equivalent, int coverage_threshold_pct);

   /* Populate css_migration_units for `project` from the component join (one
    * unit per component file, with token coverage). Existing units keep their
    * state; new units start 'pending'. Returns the unit count, -1 on error. */
   int db2_css_migration_enumerate(const char *project);

   /* Update a unit's state (+ optional oracle verdict and note). updated_at is
    * stamped by the caller-supplied timestamp (now_utc-style). */
   int db2_css_migration_set_state(const char *project, const char *unit_path, const char *state,
                                   int oracle_equivalent, const char *note, const char *now_iso);

   /* List units for a project (optionally filtered by state). */
   int db2_css_migration_list(const char *project, const char *state_filter,
                              css_migration_unit_t *out, int max);

   /* Emit a degraded-#2 convention rules document for the exemplar project,
    * derived from its indexed style graph (rule/selector/token stats + detected
    * naming convention). Returns bytes written (excl. NUL), -1 on error. */
   int db2_css_migration_rules_doc(const char *exemplar_project, char *buf, size_t cap);

   /* #2-UPGRADE: promote the machine-derivable conventions (naming scheme, token
    * strategy) from the exemplar's style graph into TYPED FACTS — assertions
    * like (project, naming_convention, "BEM") with provenance + contradiction
    * detection, via the typed-fact layer. Gated by typed_facts_enabled (returns
    * 0, a no-op, when off — the degraded rules-doc remains the spec). Idempotent:
    * a re-run supersedes only changed conventions. Returns the number of
    * conventions asserted (or already-current), -1 on error. */
   int db2_css_migration_assert_conventions(const char *project, const char *now_iso);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CSS_MIGRATION_H */
