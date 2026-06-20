/* sweep.h: pure decision logic for the deepening sweep (Part B).
 *
 * These are the analysis primitives — no IO, no model, no orchestration — so they
 * are deterministic and unit-testable. The orchestrator (a later slice) feeds them
 * the code-index edges (via kb_client) and the settled-decision set.
 *   - sweep_seam_key / sweep_excluded: the deterministic exclusion identity.
 *   - sweep_score: the mechanical deletion test (rule-of-three + distribution +
 *     independence + shared-state).
 */
#ifndef DEC_SWEEP_H
#define DEC_SWEEP_H 1

#include "aimee.h" /* MAX_PATH_LEN */

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SWEEP_KEY_MAX (MAX_PATH_LEN + 160)

   /* Canonical exclusion identity for a seam: "<file>:<top-decl>". Keyed on the
    * ORIGINAL seam (not any proposed new module), so renaming the artifact cannot
    * dodge exclusion. Writes a NUL-terminated key into out[SWEEP_KEY_MAX]. */
   void sweep_seam_key(const char *seam_file, const char *seam_symbol, char *out, size_t cap);

   /* 1 if seam_key exactly equals any entry in settled[0..n) (exact, not fuzzy). */
   int sweep_excluded(const char *seam_key, const char *const *settled, int n);

   typedef enum
   {
      SWEEP_REJECT = 0, /* not a real seam (too few/inflated callers) */
      SWEEP_WORTH,      /* worth-exploring -> filed as needs-manual */
      SWEEP_STRONG      /* clean cross-site seam */
   } sweep_rank_t;

   /* Reproduced edges for a candidate seam (from the code index). */
   typedef struct
   {
      int caller_count;   /* total callers of the proposed seam */
      int distinct_files; /* distinct files those callers live in */
      int shared_state;   /* shared deps beyond the proposed interface (blast radius) */
      int common_caller;  /* 1 if callers funnel through a common caller within N */
   } sweep_edges_t;

   typedef struct
   {
      int min_callers;            /* default 3 (the "rule of three") */
      int min_distinct_files;     /* default 2 (distribution) */
      int shared_state_tolerance; /* default 1 */
   } sweep_score_cfg_t;

   /* Fill cfg with the defaults. */
   void sweep_score_cfg_defaults(sweep_score_cfg_t *cfg);

   /* The mechanical deletion test. STRONG only when the count clears the threshold
    * AND the callers are distributed (>= min_distinct_files) AND independent
    * (no common caller) AND shared state is within tolerance; a candidate that
    * clears the count but fails a quality predicate is demoted to WORTH (never
    * silently STRONG); below the count is REJECT. Writes a short reason. */
   sweep_rank_t sweep_score(const sweep_edges_t *e, const sweep_score_cfg_t *cfg, char *reason,
                            size_t rcap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SWEEP_H */
