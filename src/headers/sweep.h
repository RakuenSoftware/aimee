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
#include "index.h" /* caller_hit_t */

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

   /* Reduce a reproduced caller set (+ blast-radius dependency count) to the
    * edges the deletion test scores. distinct_files = distinct caller file paths;
    * common_caller = 1 when every caller shares one calling function (a funnel);
    * shared_state = blast_deps. Pure. */
   sweep_edges_t sweep_edges_from_callers(const caller_hit_t *callers, int n, int blast_deps);

   /* The mechanical deletion test. STRONG only when the count clears the threshold
    * AND the callers are distributed (>= min_distinct_files) AND independent
    * (no common caller) AND shared state is within tolerance; a candidate that
    * clears the count but fails a quality predicate is demoted to WORTH (never
    * silently STRONG); below the count is REJECT. Writes a short reason. */
   sweep_rank_t sweep_score(const sweep_edges_t *e, const sweep_score_cfg_t *cfg, char *reason,
                            size_t rcap);

   /* --- scope → areas + caps (PR-B2) --- */

   /* 1 if `path` matches any allowlist glob. Globs are gitignore-style prefixes:
    * a dir followed by slash-star-star matches any path under that dir; a trailing
    * star is a prefix match; otherwise an exact match. Never trusts sub-agent
    * output — the caller passes a configured allowlist. */
   int sweep_path_allowed(const char *path, const char *const *globs, int nglobs);

   typedef struct
   {
      int max_areas;          /* default 40 */
      int max_files_per_area; /* default 50 */
      int max_calls_per_area; /* default 2 (proposer + 1 retry; verify is in-process) */
      int max_items_per_area; /* default 10 */
      int wall_area_s;        /* default 60 */
      int wall_sweep_s;       /* default 1800 */
   } sweep_caps_t;

   void sweep_caps_defaults(sweep_caps_t *caps);

   /* Partition `paths` into areas by directory, chunking any directory larger than
    * max_files_per_area. Writes an area id (0-based) per path into out_area and
    * returns the area count. `paths` MUST be sorted (same-directory files
    * contiguous) so the pass is deterministic and O(n). Returns -1 on bad args. */
   int sweep_partition(const char *const *paths, int n, int max_files_per_area, int *out_area);

   /* --- proposer-candidate parsing (PR-B3) --- */

   typedef struct
   {
      char seam_file[MAX_PATH_LEN]; /* the ORIGINAL duplicated seam's file */
      char seam_symbol[128];        /* the seam's top-level decl name */
      int claimed_callers;          /* the proposer's claimed call-site count */
      char rationale[256];          /* one-line why (opaque, for the report only) */
   } sweep_candidate_t;

   /* Parse a proposer model response (tolerant of ``` fences) into candidates.
    * Reads the "candidates" array of objects {seam_file, seam_symbol,
    * claimed_callers, rationale}; an item missing seam_file or seam_symbol is
    * skipped (the proposer must name a concrete seam). Returns the count written
    * (<= max), or -1 on parse failure. Pure (no IO / no model). */
   int sweep_parse_candidates(const char *json_text, sweep_candidate_t *out, int max);

   /* --- filing-path safety (PR-B4) --- */

   /* 1 if `path` is a safe repo-relative path to reference in a filed work item:
    * non-empty, not absolute, no ".." component, and only [A-Za-z0-9._/-] (so no
    * shell metacharacters, NUL, whitespace, globs, or quoting). The first line of
    * the untrusted-candidate -> work-item gate; the filer adds a realpath/under-root
    * check on top. Pure (lexical, no IO). */
   int sweep_path_safe(const char *path);

   /* --- delta-awareness (PR-B5) --- */

   /* Extract a filed proposal's seam key from its markdown header line
    * "# Deepen seam: <key>" into out[cap]. Returns 1 if found, 0 otherwise. The
    * sweep scans its own previously-filed proposals to build the exclusion set, so
    * a re-run does not re-file an already-filed seam ("delta-aware"). Pure. */
   int sweep_extract_seam_key(const char *proposal_md, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SWEEP_H */
