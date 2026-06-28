#ifndef AIMEE_CROSS_REPO_DEPS_H
#define AIMEE_CROSS_REPO_DEPS_H

#include "cross_repo_classify.h"
#include "cross_repo_resolver.h"

#include <stddef.h>
#include <stdint.h>

#include "index.h" /* MAX_PATH_LEN */

/* S4a orchestration: canonical_index_cross_repo_deps ties the S3 candidate
 * generation + repo descriptors to the pure S2a resolver + S2b classifier and
 * emits repo-level cross-repo dependency edges with evidence + version stamps.
 * AMBIGUOUS candidates route to the review queue (S4b). Postgres-backed; on the
 * sqlite shim the candidate-gen returns 0 rows and the call degrades to an empty
 * result. See docs/proposals/pending/cross-repo-dependency-graph.md §3.7/§3.9/§4.2.
 *
 * resolver_version: bump when the resolver/classifier logic changes so a stamped
 * edge is reproducible against the exact code that produced it. */
#define XREPO_RESOLVER_VERSION 1

/* Direction of the dependency query (§4). */
typedef enum
{
   XREPO_DIR_OUT = 0, /* deps OF `project` (project -> B) */
   XREPO_DIR_IN,      /* dependents (A -> project) */
   XREPO_DIR_BOTH
} xrepo_direction_t;

/* One emitted repo-level cross-repo edge: caller_repo depends on definer_repo,
 * via `symbol_count` distinct linking symbols, at confidence `tier`. Carries a
 * representative example + the §3.9 evidence/version stamp. */
typedef struct
{
   char caller_repo[128];
   char definer_repo[128];
   xrepo_tier_t tier;
   int symbol_count;        /* distinct linking symbols at >= the emitted tier */
   int call_site_count;     /* total call sites across those symbols */
   int import_corroborated; /* >=1 symbol took the import route (§3.1a) */
   int export_corroborated; /* >=1 symbol took the trusted-export route (§3.1b) */
   char example_symbol[128];
   char example_file[MAX_PATH_LEN];
   int example_line;
   /* recall R2c: evidence class — "symbol_resolved" (the H1-H7 symbol path),
    * "build_declared" (a build dep: FetchContent/submodule/Cargo, no symbol
    * corroboration), or "both". build_kind is the declaration kind for a build
    * edge (fetchcontent|submodule|manifest), "" for symbol-only. */
   char evidence_type[16];
   char build_kind[16];
   /* version stamp (§4.1): identifies the inputs that produced this edge. */
   char repo_set_hash[24];
   int distinctiveness_v;
   int64_t blocked_symbols_version;
   int resolver_version;
} xrepo_dep_edge_t;

/* Options for a cross-repo deps query. */
typedef struct
{
   xrepo_direction_t direction;
   xrepo_tier_t min_tier; /* emit edges at >= this tier (default MEDIUM) */
   int max_candidates;    /* candidate cap (§4.2); <=0 uses the config default */
   int include_review;    /* 1 = also write AMBIGUOUS candidates to the review queue (S4b) */
} xrepo_deps_opts_t;

/* Parse a module/package id from a manifest file's content (pure; used to build
 * repo descriptors, and unit-tested directly). `basename` selects the format:
 * "go.mod" (module line), "Cargo.toml" ([package] name), "package.json" ("name"),
 * "pyproject.toml" ([project] name). Writes the id into out (empty if absent).
 * Returns 1 if an id was found, 0 otherwise. */
int xrepo_parse_module_id(const char *basename, const char *content, char *out, size_t cap);

/* Resolve cross-repo dependency edges for `project`. Fills a heap-allocated array
 * into *out_edges (caller frees with free()), the count into *out_n, and sets
 * *truncated when the candidate cap was hit (§4.2 partial-result contract).
 * Returns 0 on success, -1 on error (out_edges set to NULL, out_n to 0). */
int canonical_index_cross_repo_deps(const char *project, const xrepo_deps_opts_t *opts,
                                    xrepo_dep_edge_t **out_edges, size_t *out_n, int *truncated);

#endif /* AIMEE_CROSS_REPO_DEPS_H */
