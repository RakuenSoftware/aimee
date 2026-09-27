#ifndef AIMEE_CROSS_REPO_REVIEW_H
#define AIMEE_CROSS_REPO_REVIEW_H

#include <stddef.h>
#include <stdint.h>

/* S4b: the AMBIGUOUS cross-repo review queue + adjudication (§3.8). Persists
 * candidates the resolver could not confidently tier (multi-definer without
 * corroboration, FFI, etc.) so they are surfaced, not dropped. Postgres-backed;
 * portable SQL so it runs on the sqlite test shim.
 * See docs/proposals/pending/cross-repo-dependency-graph.md §3.8. */

typedef struct
{
   int64_t id;
   char symbol[128];
   char caller_repo[128];
   char candidate_definer[128];
   char review_class[16]; /* 'ambiguous' | 'ffi' */
   double evidence_score;
   int cross_lang;
   char status[16]; /* 'open' | 'accepted' | 'rejected' */
   char evidence[1024];
} xrepo_review_row_t;

/* Upsert an AMBIGUOUS candidate by its fingerprint
 * (repo_set_hash, symbol, caller_repo, candidate_definer): inserts a new 'open'
 * row (arrival_seq = max+1) or refreshes the evidence/score of an existing one
 * (preserving arrival_seq + status). On overflow (> queue_max 'open' rows) evicts
 * the lowest (evidence_score ASC, arrival_seq ASC) and bumps
 * cross_repo_meta.review_overflow_dropped (never silent). Returns 0, -1 on error. */
int db2_cross_repo_review_upsert(const char *repo_set_hash, const char *symbol,
                                 const char *caller_repo, const char *candidate_definer,
                                 const char *evidence_json, double evidence_score,
                                 const char *review_class, int cross_lang, int queue_max);

/* List queued rows, highest-evidence first. `status` filters (NULL/"" -> 'open');
 * `caller_repo` NULL/"" -> all callers. Fills up to `max` rows, returns the count
 * (-1 on error). *overflow_dropped receives the cumulative evicted count (the
 * overflow.dropped indicator); may be NULL. */
int db2_cross_repo_review_list(const char *caller_repo, const char *status, xrepo_review_row_t *out,
                               int max, int64_t *overflow_dropped);

/* Adjudicate a queued row: status must be 'accepted' or 'rejected'. Idempotent
 * (re-setting the same status is a no-op success). Returns 0, -1 on error. */
int db2_cross_repo_review_set_status(int64_t id, const char *status);

#endif /* AIMEE_CROSS_REPO_REVIEW_H */
