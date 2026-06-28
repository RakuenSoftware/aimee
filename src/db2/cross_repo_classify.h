#ifndef AIMEE_CROSS_REPO_CLASSIFY_H
#define AIMEE_CROSS_REPO_CLASSIFY_H

#include "cross_repo_resolver.h"

/* Pure, DB-free tier classification for the cross-repo dependency resolver
 * (S2b): definition multiplicity (§3.5) + the deterministic gate/cap/producer
 * pipeline (§3.10). Builds on the S2a import-resolution + distinctiveness core.
 * Deterministic and DB-free, so unit-testable on the sqlite shim; S3 gathers the
 * inputs and S4 wires this into canonical_index_cross_repo_deps.
 *
 * See docs/proposals/pending/cross-repo-dependency-graph.md. */

/* Confidence tiers, ordered so the pipeline's caps are a monotone MIN over the
 * HIGH..LOW ladder (§3.10). NONE = excluded (no edge); AMBIGUOUS = routed to the
 * review queue (§3.8), off the ladder; UNIMPLEMENTED = out-of-scope language
 * (§3.7), surfaced not silently dropped. */
typedef enum
{
   XREPO_TIER_NONE = 0,
   XREPO_TIER_AMBIGUOUS,
   XREPO_TIER_LOW,
   XREPO_TIER_MEDIUM,
   XREPO_TIER_HIGH,
   XREPO_TIER_UNIMPLEMENTED
} xrepo_tier_t;

const char *xrepo_tier_name(xrepo_tier_t t);

/* ---- definition multiplicity (§3.5) -------------------------------------- *
 * Distinguish a genuine name-clash (same leaf, unrelated defs in >=2 repos ->
 * AMBIGUOUS) from polymorphic multiplicity (trait impls / method sets / overloads
 * -> intra-repo, must NOT inflate the definer count). A definition signature is
 * (qualified receiver/self type, arity, parameter types) where available; the
 * per-language dispatch matrix decides whether the receiver type is part of the
 * key. Counting is over unique DEFINER REPOS, not raw rows. */

typedef struct
{
   const char *repo;        /* defining repo name */
   xrepo_lang_t lang;       /* definition language */
   const char *self_type;   /* qualified receiver/self/trait type; "" if free function */
   int arity;               /* parameter count; -1 if unknown */
   const char *param_types; /* normalized parameter type list; "" if unknown */
   int vendored;            /* §4: 1 if this repo's defs of the symbol are ALL in vendored
                               files (no canonical copy); 0 if a non-vendored def exists */
} xrepo_def_t;

typedef enum
{
   XREPO_MULT_SINGLE = 0, /* one definer repo (or all defs polymorphic in one repo) */
   XREPO_MULT_DOMINANT,   /* a dominant definer per the hysteresis rule */
   XREPO_MULT_NAMECLASH   /* unrelated defs across >=2 repos -> AMBIGUOUS w/o corroboration */
} xrepo_mult_kind_t;

typedef struct
{
   xrepo_mult_kind_t kind;
   int definer_repos;  /* count of distinct defining repos */
   int dominant_index; /* defs[] index of the dominant definer; -1 if none */
} xrepo_mult_t;

/* Per-language dispatch config: thresholds for the dominant-definer hysteresis
 * (§3.5). dominant iff a single definer holds >= dom_share_pct% of defs AND the
 * runner-up holds <= runnerup_share_pct% AND <= runnerup_abs defs AND no other
 * definer is itself a distinctive exporter. */
typedef struct
{
   int dom_share_pct;      /* default 90 */
   int runnerup_share_pct; /* default 5 */
   int runnerup_abs;       /* default 2 */
} xrepo_mult_cfg_t;

/* Classify multiplicity over a symbol's definitions. `def_counts[i]` is the
 * number of definitions of the symbol in defs[i]'s repo (defs[] is one row per
 * distinct definer repo, already de-duplicated by the caller); `exporter[i]` is
 * 1 if that repo distinctively exports the symbol. The per-language dispatch
 * (whether self_type participates) is applied when deciding name-clash vs
 * polymorphic. */
xrepo_mult_t xrepo_classify_multiplicity(const xrepo_def_t *defs, const int *def_counts,
                                         const int *exporter, int n, const xrepo_mult_cfg_t *cfg);

/* ---- the deterministic tier pipeline (§3.10) ----------------------------- */

/* Corroboration route that sets the base tier (the pipeline's only producer). */
typedef enum
{
   XREPO_CORROB_NONE = 0,       /* no corroboration -> LOW */
   XREPO_CORROB_DOMINANT,       /* dominant single definer -> MEDIUM */
   XREPO_CORROB_TRUSTED_EXPORT, /* §3.1b export route (trusted definer) -> HIGH */
   XREPO_CORROB_IMPORT          /* §3.1a import resolution -> HIGH */
} xrepo_corrob_t;

/* Everything the pipeline needs about one candidate edge. The caller (S4) fills
 * this from the S2a resolver result + S3 stats; the pipeline itself is pure. */
typedef struct
{
   xrepo_lang_t lang;            /* caller-site language; UNKNOWN -> UNIMPLEMENTED */
   int originated_in_caller;     /* S has an ORIGINAL (non-re-export) def in A -> no edge */
   int distinctive;              /* xrepo_name_distinctive result (§3.3) */
   xrepo_mult_t mult;            /* §3.5 multiplicity result */
   int caller_collision;         /* S behaves like a local method in A (§3.4) -> one-tier cap */
   xrepo_corrob_t corroboration; /* base-tier producer (§3.1) */
   int caller_trusted;           /* caller repo trust (§0): untrusted IMPORT route caps at MEDIUM */
   int definer_trusted;          /* definer repo trust (§0): untrusted EXPORT route caps at MEDIUM
                                  * (an untrusted definer can never lend HIGH export) */
   xrepo_import_modality_t modality; /* conditional -> one-tier cap; dynamic -> review */
} xrepo_candidate_t;

/* Per-stage trace, for the --dry-run evidence + structural-invariant asserts. */
typedef struct
{
   xrepo_tier_t tier;            /* final tier (or AMBIGUOUS/NONE/UNIMPLEMENTED) */
   int routed_to_review;         /* 1 = AMBIGUOUS -> review queue */
   xrepo_tier_t base_tier;       /* producer output before caps */
   int caller_collision_applied; /* a cap lowered the tier */
   int trust_cap_applied;
   int modality_cap_applied;
   const char *reason; /* short stage label that determined the outcome */
} xrepo_classification_t;

/* Run the §3.10 pipeline. Deterministic: a fixed candidate always yields the
 * same classification. Gates (invariant, distinctiveness, multiplicity) may
 * short-circuit to NONE / AMBIGUOUS; otherwise the producer sets a base tier and
 * the caps take the MIN (caller-collision, trust, conditional-import each lower
 * by at most one tier, floored at LOW; dynamic-import routes to review). */
xrepo_classification_t xrepo_classify(const xrepo_candidate_t *c);

/* Eviction score for the AMBIGUOUS review queue (§3.8): higher = more evidence,
 * so the queue evicts the lowest first. Independent of the (absent) tier so it is
 * well-defined for AMBIGUOUS rows. */
double xrepo_evidence_score(int distinctiveness_rank, int call_site_count,
                            int corroboration_routes);

#endif /* AIMEE_CROSS_REPO_CLASSIFY_H */
