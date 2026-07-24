/* kb_rrf.h: Reciprocal Rank Fusion over ranked candidate lists.
 *
 * Shared, not kb-owned. The implementation is pure -- no DB, no network, no
 * kb state -- and the web-search path fuses engine rankings with it, which the
 * module boundary rightly forbids reaching into kb/ to do. Originally written
 * for hybrid code retrieval (proposal §5); the name is kept so the kb callers
 * and their history stay greppable.
 *
 * Fuses several independently-ranked candidate lists — graph neighborhood,
 * vector similarity, memory recall — into one ranking. The three signals have
 * non-comparable raw scores (hop counts vs cosine vs recency), so we fuse by
 * RANK, not raw score: a candidate's fused score is
 *     Σ_signals  weight_s / (k + rank_s(d))
 * summed only over the signals that actually contain it (rank_s is 1-based).
 * RRF needs no score normalization, is robust to a signal being absent (the
 * candidate simply isn't in that list), and degrades gracefully. Ties break on
 * the structural-trust weight of the connecting edge, then on id — fully
 * deterministic. Pure: no DB / network / allocation-of-the-caller's-data, so it
 * unit-tests standalone and is reused by the /v1/code hybrid route. */
#ifndef KB_RRF_H
#define KB_RRF_H

/* RRF's standard rank constant (k); dampens the contribution of low ranks. */
#define KB_RRF_DEFAULT_K 60.0

/* One candidate emitted by a single signal. Its position in the signal's array
 * IS its rank (index 0 = the signal's top hit). `id` MUST be NUL-terminated — the
 * fusion compares it with strcmp and copies it with snprintf; an empty id is
 * skipped. */
typedef struct
{
   char id[256];          /* opaque candidate key, NUL-terminated: file_path, symbol, ... */
   int structural_weight; /* structural trust of the connecting edge (tie-break) */
} kb_rrf_item_t;

/* A ranked candidate list from one signal, with its fusion weight w_s. */
typedef struct
{
   const kb_rrf_item_t *items;
   int count;
   double weight;
   const char *label; /* optional signal name (e.g. "graph"); may be NULL */
} kb_rrf_signal_t;

/* A fused candidate. */
typedef struct
{
   char id[256];
   double score;
   int structural_weight; /* max structural_weight seen for this id across signals */
   int signal_hits;       /* how many signals contributed this candidate */
} kb_rrf_result_t;

/* Fuse `n` ranked signal lists into out[] (capacity `max`), sorted by fused
 * score desc, tie-broken by structural_weight desc then id asc. `k` is the RRF
 * constant (> 0; pass KB_RRF_DEFAULT_K). A signal with weight <= 0 or count <= 0
 * is skipped. Returns the number of distinct candidates written (<= max), or -1
 * on a bad argument. Never allocates caller-visible memory; out[] is caller-owned. */
int kb_rrf_fuse(const kb_rrf_signal_t *signals, int n, double k, kb_rrf_result_t *out, int max);

/* A per-candidate earned-trust value from the §3 lessons artifact, keyed by the
 * same id space as the signals. */
typedef struct
{
   char id[256];
   double trust; /* signed; higher = more trusted (from lessons_reflect scores) */
} kb_rrf_trust_t;

/* Fuse as kb_rrf_fuse, but with earned trust inserted as a TIE-BREAK ONLY
 * (proposal §3, v1): among candidates with an EXACTLY-equal fused score AND equal
 * structural_weight, the higher-trust one ranks first (then id asc). Trust never
 * moves a candidate across a real score gap — RRF's rank-distance blend stays the
 * primary signal and earned trust only nudges within a genuine tie. `trust`/
 * `n_trust` is an unordered lookup; a candidate absent from it (or `trust==NULL`)
 * is treated as trust 0, so passing NULL makes this byte-identical to kb_rrf_fuse.
 * Same return contract; deterministic. */
int kb_rrf_fuse_trust(const kb_rrf_signal_t *signals, int n, double k, const kb_rrf_trust_t *trust,
                      int n_trust, kb_rrf_result_t *out, int max);

#endif /* KB_RRF_H */
