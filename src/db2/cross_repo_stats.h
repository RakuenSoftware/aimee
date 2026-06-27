#ifndef AIMEE_CROSS_REPO_STATS_H
#define AIMEE_CROSS_REPO_STATS_H

#include "cross_repo_classify.h"
#include "cross_repo_resolver.h"

#include <stddef.h>
#include <stdint.h>

/* DB-backed stats / data-gathering for the cross-repo resolver (S3). These run
 * over db2 (Postgres in production; the sqlite test shim where the SQL is kept
 * portable -- plain SELECT/GROUP BY/ON CONFLICT, no JSONB/to_char) and feed the
 * pure S2a/S2b core. S4 composes candidate generation + classification.
 *
 * Trust contract (§0): every frequency / candidate query carries the per-repo
 * trust column; distinctiveness + blocked_symbols are computed over TRUSTED
 * repos only, while candidate generation still surfaces untrusted edges (the
 * pure pipeline applies the per-edge trust cap). Functions return -1 on a DB
 * error / no connection (the caller degrades gracefully, never crashes).
 *
 * Cross-slice freshness (§4.1): a projects.trust flip changes which repos feed
 * the frequency model, so blocked_symbols becomes stale. The trust-write
 * transaction (S7) is responsible for bumping trust_epoch AND scheduling a
 * recompute (or invalidating blocked_symbols_version); these stats functions do
 * not observe trust changes on their own.
 *
 * Scale (§4.2): these are per-symbol scans over code_calls/terms. S4a does NOT
 * call them per-candidate; it builds the cached dominant-definer/exports working
 * set once per request (the two-step query) and reuses it.
 *
 * See docs/proposals/pending/cross-repo-dependency-graph.md §3.3/§3.7/§4.1. */

/* §3.3 distinctiveness stats for symbol S as used by caller repo A, computed over
 * TRUSTED repos only: callee_repo_count (# trusted repos where S is a callee),
 * definer_repo_count (# trusted repos defining S), caller_file_pct (% of A's
 * files where S is a callee). Fills *out. Returns 0 on success, -1 on error. */
int db2_cross_repo_distinct_stats(const char *symbol, const char *caller_repo,
                                  xrepo_distinct_stats_t *out);

/* §3.3 recompute the blocked_symbols set over TRUSTED repos: a symbol is blocked
 * when it is a callee in >= k repos OR defined in >= m repos (length >= len_min
 * is required to be considered distinctive, so shorter names are implicitly
 * blocked at query time, not stored). Replaces the table contents and bumps
 * cross_repo_meta.blocked_symbols_version. Returns rows written, -1 on error. */
int db2_cross_repo_recompute_blocked_symbols(int k, int m, int len_min);

/* §3.3 membership test against the materialized blocked_symbols set for `lang`
 * (and the lang='' all-languages rows). Returns 1 = blocked, 0 = not, -1 error. */
int db2_cross_repo_symbol_blocked(const char *symbol, const char *lang);

/* §4.1 per-repo symbol-table hash: FNV-1a over the repo's canonicalized
 * (terms,file_exports,file_imports) rows. out buffer must be >= 17 bytes
 * (16 hex + NUL). Returns 0 on success, -1 on error. */
int db2_cross_repo_repo_symbol_hash(const char *project, char *out, size_t cap);

/* §4.1 repo_set_hash: FNV-1a over the sorted (name,trust,symbol-table-hash) of
 * all registered repos; also written into cross_repo_meta.repo_set_hash. out
 * buffer must be >= 17 bytes. Returns 0 on success, -1 on error. */
int db2_cross_repo_repo_set_hash(char *out, size_t cap);

/* Read the current global version stamp components (§4.1) from cross_repo_meta:
 * trust_epoch, blocked_symbols_version, and the stored repo_set_hash. Any out
 * pointer may be NULL. Returns 0 on success, -1 on error. */
int db2_cross_repo_meta_read(int64_t *trust_epoch, int64_t *blocked_symbols_version,
                             char *repo_set_hash, size_t cap);

/* NOTE: repo descriptors (manifest module_id parsing for the §3.7 resolver) and
 * the §4.2 candidate-generation query are orchestration-coupled and validated
 * live; they land with S4a (canonical_index_cross_repo_deps), not in this slice. */

#endif /* AIMEE_CROSS_REPO_STATS_H */
