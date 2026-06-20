/* evidence_replay.h: deterministic replay of a review item's structured evidence
 * (Part A of the replayable-verification proposal).
 *
 * A roundtable panelist attaches a STRUCTURED query (review_evidence_t) — never a
 * free-form command. This engine re-grounds that query against the read-only code
 * index (index_find / index_find_callers / index_code_search) and reduces the
 * result to a fixed-shape record { count, idkey } that a verifier reasons over.
 * The raw index rows never leave this layer (no model ever sees them), and the
 * index can only reference files from scanned projects — so there is no shell, no
 * arbitrary path, and containment is by construction.
 *
 * Index access goes through a small backend vtable so the engine is unit-testable
 * with a fake (no live DB); evidence_replay() uses the real index.
 */
#ifndef DEC_EVIDENCE_REPLAY_H
#define DEC_EVIDENCE_REPLAY_H 1

#include "index.h"             /* aimee.h (MAX_PATH_LEN), term_hit_t, caller_hit_t, ... */
#include "delegate_ensemble.h" /* review_evidence_t, ev_kind_t */

#ifdef __cplusplus
extern "C"
{
#endif

#define REPLAY_IDKEY_HEX 65 /* sha256 lowercase hex (64) + NUL */

   typedef enum
   {
      REPLAY_MATCH = 0,        /* reproduced exactly as claimed */
      REPLAY_CORRECTED,        /* reproduced, but actual count != claimed (re-grounded to actual) */
      REPLAY_CONTRADICTED,     /* index populated, claim does not reproduce -> reject */
      REPLAY_NO_EVIDENCE,      /* EV_NONE: interpretive item, nothing to replay (keep, cap) */
      REPLAY_VACUOUS,          /* non-NONE kind but empty/malformed query -> reject */
      REPLAY_INDEX_UNAVAILABLE /* DB error or no project indexed -> DEGRADE (keep, unverified) */
   } replay_status_t;

   typedef struct
   {
      int count;                    /* reproduced count (hits / callers / matches) */
      char idkey[REPLAY_IDKEY_HEX]; /* sha256 of sorted "file:line" set, "" if empty */
   } reduced_record_t;

   /* Index backend — defaults wrap the real index_*; tests inject a fake.
    * Each find_* returns count >= 0, or -1 on DB/connection error.
    * project_count returns the number of indexed projects (0 => index empty). */
   typedef struct
   {
      int (*find_symbol)(const char *identifier, term_hit_t *out, int max);
      int (*find_callers)(const char *project, const char *symbol, caller_hit_t *out, int max);
      int (*code_search)(const char *query, const char *project, code_search_hit_t *out, int max);
      int (*project_count)(void);
   } replay_backend_t;

   /* Register the process-wide replay backend (a weak seam). The server installs
    * a kb_client-backed backend at startup; contexts without a code index (CLI,
    * gateway, tests that don't set one) leave it NULL, so evidence_replay()
    * DEGRADES (INDEX_UNAVAILABLE) rather than referencing any index symbol. `be`
    * must outlive its use (pass a static). */
   void evidence_replay_set_backend(const replay_backend_t *be);

   /* The currently-registered backend, or NULL. */
   const replay_backend_t *evidence_replay_active_backend(void);

   /* Replay `ev` against the registered backend (NULL backend => degrade). */
   replay_status_t evidence_replay(const review_evidence_t *ev, reduced_record_t *out);

   /* Replay against an explicit backend (for tests). A NULL `be` degrades
    * (INDEX_UNAVAILABLE) — there is no index to ground against. */
   replay_status_t evidence_replay_with(const replay_backend_t *be, const review_evidence_t *ev,
                                        reduced_record_t *out);

   /* Stable lowercase token for a status (for audit logs / reason codes). */
   const char *replay_status_str(replay_status_t s);

   /* sha256-hex of `n` "file:line" strings, sorted ascending, joined by '\n'.
    * Writes into out[REPLAY_IDKEY_HEX]. Exposed for tests (determinism). */
   void evidence_idkey(const char (*files)[MAX_PATH_LEN], const int *lines, int n,
                       char out[REPLAY_IDKEY_HEX]);

#ifdef __cplusplus
}
#endif

#endif /* DEC_EVIDENCE_REPLAY_H */
