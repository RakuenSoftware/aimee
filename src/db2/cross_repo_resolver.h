#ifndef AIMEE_CROSS_REPO_RESOLVER_H
#define AIMEE_CROSS_REPO_RESOLVER_H

#include <stddef.h>

/* Pure, DB-free core of the cross-repo dependency resolver.
 *
 * S2a covers import resolution (§3.7) + distinctiveness (§3.3); the tier
 * pipeline + multiplicity (§3.5/§3.10) land in S2b. Every function here is
 * deterministic and takes precomputed corpus data as input -- it never touches
 * the database -- so it is fully unit-testable on the sqlite shim (where the
 * Postgres-only db2 ops return -1). The DB layer (S3) gathers the inputs.
 *
 * See docs/proposals/pending/cross-repo-dependency-graph.md. */

/* P1 in-scope languages (§3.7). Anything else -> XREPO_LANG_UNKNOWN, which the
 * resolver surfaces as the UNIMPLEMENTED tier rather than silently omitting. */
typedef enum
{
   XREPO_LANG_UNKNOWN = 0,
   XREPO_LANG_C,
   XREPO_LANG_CPP,
   XREPO_LANG_RUST,
   XREPO_LANG_GO,
   XREPO_LANG_TS,
   XREPO_LANG_JS,
   XREPO_LANG_PYTHON
} xrepo_lang_t;

/* Classify a source path by extension (deterministic; no I/O). */
xrepo_lang_t xrepo_lang_from_path(const char *path);
/* Stable lowercase tag ("c","cpp","rust","go","ts","js","python","unknown"). */
const char *xrepo_lang_name(xrepo_lang_t lang);

/* Import modality (§3.7): static imports can reach HIGH; conditional ones
 * downgrade HIGH->MEDIUM; dynamic ones are routed to the review queue and never
 * reach static HIGH. The caller (S3/S4) determines the modality from the
 * import's guard context and passes it in. */
typedef enum
{
   XREPO_IMPORT_STATIC = 0,
   XREPO_IMPORT_CONDITIONAL,
   XREPO_IMPORT_DYNAMIC
} xrepo_import_modality_t;

/* A registered repo descriptor for import resolution. The DB layer (S3) fills
 * these and owns the normalization contract for `module_id`:
 *   - Rust:   the crate name (Cargo.toml [package].name, '-' kept as written;
 *             the resolver compares the import's first `::` token verbatim).
 *   - Go:     the module path (go.mod `module`), e.g. "example.com/foo".
 *   - TS/JS:  the npm package name (package.json "name"), incl. "@scope/name".
 *   - Python: the top-level import package (dist/top_level), e.g. "requests".
 *   - "" when the repo exposes no module id (C/C++ repos resolve by `headers`).
 * `trusted` mirrors projects.trust (§0): only trusted repos can lend HIGH
 * corroboration. */
typedef struct
{
   const char *name;           /* repo (project) name */
   int trusted;                /* 1 = trusted, 0 = untrusted */
   const char *module_id;      /* normalized per the contract above; "" if none */
   const char *const *headers; /* C/C++ only: indexed header paths to suffix-match;
                                * NULL/0 for other languages */
   size_t header_count;
} xrepo_repo_desc_t;

/* Max colliding definer repos enumerated on a MANY result (for the AMBIGUOUS
 * review queue evidence). collision_count may exceed this; collisions[] holds
 * the first XREPO_MAX_COLLISIONS by descriptor order. */
#define XREPO_MAX_COLLISIONS 8

/* Resolution cardinality (§3.7 cardinality contract): the resolver returns
 * zero / one / many candidate repos and never silently picks one of several. */
typedef enum
{
   XREPO_RESOLVE_NONE = 0, /* import route unavailable (MEDIUM still reachable) */
   XREPO_RESOLVE_ONE,      /* exactly one -> import route (HIGH(a)) can fire */
   XREPO_RESOLVE_MANY      /* many -> routed to AMBIGUOUS, never guessed */
} xrepo_resolve_card_t;

typedef struct
{
   xrepo_resolve_card_t cardinality;
   int repo_index;                       /* index into descs[] when cardinality==ONE; else -1 */
   int system_header;                    /* C/C++: include hit a rejected system/framework header */
   xrepo_import_modality_t modality;     /* echoed through for the pipeline's import-modality cap */
   int collisions[XREPO_MAX_COLLISIONS]; /* MANY: colliding definer descs[] indices */
   int collision_count;                  /* MANY: total colliders (may exceed the array) */
} xrepo_resolve_result_t;

/* Map a raw import/include string in `lang`, used by `caller_repo`, to a definer
 * repo among descs[]. Pure + deterministic.
 *   - C/C++: longest path-suffix of the #include that resolves to exactly one
 *     indexed header in a trusted repo; system/framework headers rejected; a
 *     suffix that matches >1 indexed file -> MANY (AMBIGUOUS).
 *   - Rust:  `use crate_or_alias::...` matched to a crate's module_id; crate::/
 *     super::/self:: paths are intra-repo -> NONE.
 *   - Go:    import-path prefix matched to a module_id (go.mod).
 *   - TS/JS: bare specifier matched to a package module_id; relative -> NONE.
 *   - Python:import's top-level package matched to a module_id; relative -> NONE.
 *   - Monorepo: a sub-package path resolves to its containing registered repo.
 * The caller-supplied `modality` is echoed into the result. The caller repo is
 * excluded from the candidate set before counting, so it never yields a
 * self-edge nor inflates a MANY result. On MANY, collisions[]/collision_count
 * enumerate the colliding definer repos (for the review queue). */
xrepo_resolve_result_t xrepo_resolve_import_to_repo(const char *raw_import, xrepo_lang_t lang,
                                                    const char *caller_repo,
                                                    xrepo_import_modality_t modality,
                                                    const xrepo_repo_desc_t *descs,
                                                    size_t desc_count);

/* Distinctiveness (§3.3). S is distinctive iff NONE of:
 *   - S is a callee in >= k trusted repos, or
 *   - S is defined in >= m trusted repos, or
 *   - S is a callee in >= p_pct% of the caller repo A's files,
 * AND xrepo_utf8_len(S) >= len_min. Stats are precomputed over trusted repos
 * by S3; thresholds come from kb.curator.cross_repo_graph.* (versioned). */
typedef struct
{
   int callee_repo_count;  /* # trusted repos where S appears as a callee */
   int definer_repo_count; /* # trusted repos defining S */
   int caller_file_pct;    /* % of caller A's files where S is a callee (0..100) */
} xrepo_distinct_stats_t;

typedef struct
{
   int k;       /* callee-in->=k-repos floor */
   int m;       /* defined-in->=m-repos floor */
   int p_pct;   /* caller-file-percentage floor */
   int len_min; /* minimum UTF-8 code-point length */
} xrepo_distinct_cfg_t;

/* 1 = distinctive, 0 = not. */
int xrepo_name_distinctive(const char *symbol, const xrepo_distinct_stats_t *stats,
                           const xrepo_distinct_cfg_t *cfg);

/* UTF-8 code-point length (count of code points, not bytes) for the len_min gate. */
size_t xrepo_utf8_len(const char *s);

#endif /* AIMEE_CROSS_REPO_RESOLVER_H */
