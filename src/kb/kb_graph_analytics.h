/* kb_graph_analytics.h: graph analytics over the code projection graph
 * (proposal §4). Currently: hub / degree-centrality ranking — the most-connected
 * symbols, a refactor-risk signal ("editing this touches a lot").
 *
 * Pure: operates on an in-memory edge array, no DB/network/caller-data alloc, so
 * it unit-tests standalone and is reused by the /v1/code/graph/hubs route. */
#ifndef KB_GRAPH_ANALYTICS_H
#define KB_GRAPH_ANALYTICS_H

/* Node-name buffer. Sized to comfortably hold a path-qualified symbol name so two
 * distinct nodes don't collapse under a shared truncated prefix (which would
 * distort the ranking). Kept in sync with code_projection_edge_t's endpoints. */
#define KB_GRAPH_NODE_MAX 512

/* One directed edge of the projection graph. `source`/`target` MUST be
 * NUL-terminated (compared with strcmp, copied with snprintf); an edge with an
 * empty endpoint is skipped. `weight` is the edge's structural-trust weight
 * (>= 0); it feeds the weighted-degree tie-break. */
typedef struct
{
   char source[KB_GRAPH_NODE_MAX];
   char target[KB_GRAPH_NODE_MAX];
   int weight;
} kb_graph_edge_t;

/* A relation-typed edge — the generic kb_graph_edge_t plus the projection's
 * relation label ("calls"/"defines"/"imports"/…). Only kb_graph_cycles needs
 * relations (to collapse symbols to their defining file); the other analytics are
 * relation-agnostic, so this is kept separate rather than bloating kb_graph_edge_t.
 * Mirrors the (source, relation, target) of code_projection_edge_t. */
typedef struct
{
   char source[KB_GRAPH_NODE_MAX];
   char relation[64];
   char target[KB_GRAPH_NODE_MAX];
} kb_graph_reledge_t;

/* One ranked hub node. */
typedef struct
{
   char node[KB_GRAPH_NODE_MAX];
   int in_degree;       /* edges pointing AT this node (it is a target)   */
   int out_degree;      /* edges leaving this node (it is a source)        */
   int degree;          /* in_degree + out_degree                          */
   int weighted_degree; /* sum of incident edge weights (in + out)         */
} kb_graph_hub_t;

/* Rank the nodes of `edges` by degree centrality and write the top `max` into
 * out[], sorted by degree desc, tie-broken by weighted_degree desc then node
 * asc (fully deterministic). A self-loop (source==target) contributes one out
 * and one in to the same node. Returns the number of distinct nodes written
 * (<= max), 0 if there are no usable edges, or -1 on a bad argument. Never
 * allocates caller-visible memory; out[] is caller-owned. */
int kb_graph_hubs(const kb_graph_edge_t *edges, int n_edges, kb_graph_hub_t *out, int max);

/* ── §4 surprising links (high embedding similarity AND high graph distance) ──── */

/* A candidate node pair + its embedding cosine similarity. `a`/`b` are node ids
 * in the SAME space as the edges' source/target (so hop distance is meaningful). */
typedef struct
{
   char a[KB_GRAPH_NODE_MAX];
   char b[KB_GRAPH_NODE_MAX];
   double cosine;
} kb_graph_pair_t;

/* A surprising link: semantically close yet structurally far (or disconnected). */
typedef struct
{
   char a[KB_GRAPH_NODE_MAX];
   char b[KB_GRAPH_NODE_MAX];
   double cosine;
   int hops; /* undirected shortest-path hops over the edges; -1 = disconnected */
} kb_graph_surprising_t;

/* Bound on BFS exploration so a huge graph can't blow time/space (the result is a
 * conservative "far enough" past this — counted as disconnected). */
#define KB_GRAPH_BFS_MAX_NODES 4096

/* Undirected shortest-path hop count between `src` and `dst` over `edges` (BFS,
 * treating each edge as bidirectional). Returns 0 if src==dst, the hop count if
 * reachable within KB_GRAPH_BFS_MAX_NODES explored nodes, or -1 if disconnected /
 * either endpoint is absent / a bad argument. Pure (internal scratch only). */
int kb_graph_shortest_hops(const kb_graph_edge_t *edges, int n_edges, const char *src,
                           const char *dst);

/* Surprising-links filter (§4, R1-precise). From candidate `pairs`, keep those
 * whose cosine is at/above the `sim_percentile` (0..1) of the candidates' OWN
 * cosine distribution (data-driven, not a hardcoded constant) AND whose
 * structural hop-distance is >= `d_min` OR disconnected. Writes up to `max`,
 * ordered by cosine desc, tie-broken by larger hops (disconnected ranks highest)
 * then a asc then b asc (deterministic). Returns the count written, 0 if none
 * qualify, -1 on a bad argument. Pure; out[] is caller-owned. */
int kb_graph_surprising(const kb_graph_edge_t *edges, int n_edges, const kb_graph_pair_t *pairs,
                        int n_pairs, double sim_percentile, int d_min, kb_graph_surprising_t *out,
                        int max);

/* §4 precision self-suppress. The LLM judge samples the structural candidate
 * generator's precision: `confirmed`/`judged` is the fraction of cosine+distance
 * candidates the judge deemed genuine. Returns 1 when the surprising feature should
 * SUPPRESS its (unjudged) structural candidates because that sampled precision has
 * dropped below `floor` over a meaningful sample — i.e. the structural filter is
 * mostly surfacing false positives, so showing raw candidates would be noise. Returns
 * 0 when there isn't enough signal yet (`judged` < `min_samples`), the floor is
 * disabled (`floor` <= 0), or precision is at/above the floor. Pure. */
int kb_surprising_precision_suppress(int judged, int confirmed, int min_samples, double floor);

/* ── S-community: deterministic community detection (graph-feedback foundation) ── */

/* One node's community assignment. `community` is the community id: the
 * lexicographically-smallest member node id (see contract below). */
typedef struct
{
   char node[KB_GRAPH_NODE_MAX];
   char community[KB_GRAPH_NODE_MAX];
} kb_graph_community_t;

/* Cap on local-moving passes. Convergence is a no-move pass; this only bounds a
 * pathological graph that never settles. */
#define KB_GRAPH_COMMUNITY_MAX_PASSES 64

/* Upper bound on 2m (the summed weighted degree). The exact-integer gain forms
 * the products 2m*k_in and k_i*sigma_tot, each <= (2m)^2; keeping 2m below
 * floor(sqrt(INT64_MAX)) guarantees they never overflow int64. A graph whose
 * total weight exceeds this is rejected (-1). Real callers pass structural
 * weights <= 3 over a few thousand edges — many orders of magnitude below it. */
#define KB_GRAPH_COMMUNITY_MAX_TWO_M 3037000499LL

/* Partition the nodes of `edges` into communities by deterministic modularity
 * maximization, writing one row per distinct node into out[] (up to `max`, in
 * node-ascending order). Returns the distinct-node count written (<= max), 0 if
 * there are no usable edges, or -1 on a bad argument. Never allocates
 * caller-visible memory; out[] is caller-owned.
 *
 * NORMALIZATION SPEC (fully pinned — single method, no fallback):
 *   - Edges are treated UNDIRECTED.
 *   - Parallel edges between the same pair are AGGREGATED by summed weight.
 *   - Self-loops (source==target) are DROPPED.
 *   - Edge weight = the edge's `weight` (structural trust), clamped to >= 0; a
 *     0-weight edge is inert (present but exerts no pull).
 *   - Objective: modularity with resolution gamma = 1.0.
 *   - Method: single-level Louvain local moving. Every node starts in its own
 *     singleton. Nodes are processed in a FIXED order (node id ascending) each
 *     pass; a node stays in its current community unless a neighbouring community
 *     offers STRICTLY positive modularity gain, in which case it moves to the
 *     best such neighbour. The gain is EXACT INTEGER arithmetic (compared as
 *     2m*k_in - k_i*sigma_tot, gamma=1) so it is permutation-invariant — no
 *     floating point, no summation-order sensitivity. (A node only re-isolates at
<<<<<<< Updated upstream
 *     initialization; a merged node staying by default keeps its current
=======
 *     initialization; a merged node returning by default lands in its current
>>>>>>> Stashed changes
 *     community — standard single-level behaviour.)
 *   - Iteration: repeat passes until a pass moves nothing (convergence) or
 *     KB_GRAPH_COMMUNITY_MAX_PASSES is reached.
 *   - Total-order tie-break among the positive-gain neighbours of equal gain:
 *     pick the one whose min-member node id is lexicographically smallest, as
 *     snapshotted at the start of the current pass (a deterministic key; at
 *     convergence the snapshot equals the exact current min-member). A zero-gain
 *     neighbour never displaces the stay option, so there is no gain-free churn.
 *   - Community id = the min-member node id (lex). Ids are GENERATION-LOCAL by
 *     contract; cross-generation stable remap is a later slice (S2).
 *
 * Determinism: for a fixed node/edge set the output is byte-identical regardless
 * of the order edges are presented in. */
int kb_graph_communities(const kb_graph_edge_t *edges, int n_edges, kb_graph_community_t *out,
                         int max);

/* ── S1 self-audit: surface what the graph is unsure about (proposal §1) ─────────
 *
 * A read-only, deterministic analytic pass over the projection graph that emits
 * ranked structural-health findings. Pure (in-memory edge array only); each
 * function unit-tests standalone and is composed by the /v1/code/graph/audit
 * route. Every output list is total-ordered (score then node lex) so equal-scored
 * items never permute between runs and read as change. */

/* Hub-ranking view. TOP = most-connected (the existing refactor-risk signal).
 * BOTTOM = least-connected (the orphan view). BOTTOM_NOHUB = least-connected
 * excluding container/hub nodes (project:/file: prefixes) so a genuinely orphaned
 * *symbol* isn't crowded out by empty container nodes. One ranking, two views
 * (§1): top and bottom of the same degree distribution can't disagree. */
typedef enum
{
   KB_HUB_TOP = 0,
   KB_HUB_BOTTOM = 1,
   KB_HUB_BOTTOM_NOHUB = 2
} kb_hub_mode_t;

/* Rank nodes by degree in the requested view; otherwise identical contract to
 * kb_graph_hubs (which is now the KB_HUB_TOP wrapper). For a BOTTOM* view the
 * order is degree ASC, then weighted_degree ASC, then node asc. */
int kb_graph_hubs_ranked(const kb_graph_edge_t *edges, int n_edges, kb_graph_hub_t *out, int max,
                         kb_hub_mode_t mode);

/* True if `node` is a container/hub node (project:/file: key prefix) — excluded
 * from the orphan and bridge views because their degree is structural bookkeeping,
 * not a real symbol relationship. */
int kb_graph_is_container(const char *node);

/* ── cycles ──────────────────────────────────────────────────────────────────
 * File-level dependency cycles. The projection carries no direct file→file edge,
 * so we collapse: `defines` edges (file→symbol) map each symbol to its file, then
 * `calls` edges (symbol→symbol) become directed file→file edges (self-file calls
 * dropped). Tarjan SCC over that file graph, then a bounded DFS per non-trivial
 * SCC enumerates up to KB_AUDIT_CYCLE_CAP simple cycles (Johnson explodes on dense
 * diamonds). Truncation is reported, never silent. */
#define KB_AUDIT_CYCLE_MAX_LEN 32  /* longest cycle reported; longer ones truncated */
#define KB_AUDIT_CYCLE_CAP     100 /* max cycles enumerated per SCC */

typedef struct
{
   char files[KB_AUDIT_CYCLE_MAX_LEN][KB_GRAPH_NODE_MAX];
   int len; /* number of files in the cycle (>= 2) */
} kb_graph_cycle_t;

/* Detect file-level cycles into out[] (up to max), each a canonical-rotation
 * ordered file list. Sets *truncated to 1 if any SCC hit KB_AUDIT_CYCLE_CAP or
 * the out buffer filled. Returns the count written, 0 if acyclic, -1 on bad arg. */
int kb_graph_cycles(const kb_graph_reledge_t *edges, int n_edges, kb_graph_cycle_t *out, int max,
                    int *truncated);

/* ── bridges ─────────────────────────────────────────────────────────────────
 * High edge-betweenness nodes that are NOT container/file hubs — the cross-cutting
 * concerns connecting otherwise-separate modules. Brandes betweenness (unweighted,
 * undirected). Exact when the node count is <= KB_AUDIT_BRIDGE_EXACT_MAX; above it,
 * betweenness is estimated from a deterministic first-K lex source sample and the
 * result is marked approximate. */
#define KB_AUDIT_BRIDGE_EXACT_MAX 1500 /* exact Brandes below this many nodes */
#define KB_AUDIT_BRIDGE_SAMPLE    256  /* deterministic source sample above it */

typedef struct
{
   char node[KB_GRAPH_NODE_MAX];
   double betweenness; /* Brandes score; approximate scores are sample-scaled */
} kb_graph_bridge_t;

/* Rank non-container nodes by betweenness into out[] (up to max), desc then node
 * asc. Sets *approximate to 1 when sampling was used. Returns count written, 0 if
 * no bridges, -1 on bad arg. */
int kb_graph_bridges(const kb_graph_edge_t *edges, int n_edges, kb_graph_bridge_t *out, int max,
                     int *approximate);

/* ── low-cohesion communities ────────────────────────────────────────────────
 * A community (from kb_graph_communities) is incohesive when a large share of its
 * incident edge weight leaves the community. Scored by conductance =
 * cut(C) / min(vol(C), vol(V\C)) — NOT raw internal density, which returns ~1.0
 * for tiny communities and misreads sparse-but-valid structure. Gated at size >=
 * min_size (proposal: 8). Higher conductance = worse cohesion (a split candidate). */
typedef struct
{
   char community[KB_GRAPH_NODE_MAX];
   double conductance;
   int size; /* member count */
} kb_graph_cohesion_t;

/* Score communities and write the worst (highest-conductance) into out[] (up to
 * max), desc then community lex. `comm`/`n_comm` is the assignment from
 * kb_graph_communities over the SAME edges. Only communities of size >= min_size
 * are considered. Returns count written, 0 if none qualify, -1 on bad arg. */
int kb_graph_cohesion(const kb_graph_edge_t *edges, int n_edges, const kb_graph_community_t *comm,
                      int n_comm, int min_size, kb_graph_cohesion_t *out, int max);

/* ── S2: cross-generation community remap + snapshot diff (proposal §2) ──────────
 *
 * A snapshot diff is only useful if it shows REAL change, so two determinism traps
 * must close: community ids must be stable run-to-run (else every re-index looks
 * like churn), and every ranked/diffed list must be total-ordered. */

/* Remap the NEW generation's community ids so they are STABLE against the OLD
 * generation: a new community that best-overlaps an old community inherits that
 * old community's id, so "community X" denotes the same module across generations.
 * Deterministic two-pass (proposal §2):
 *   Pass 1 — each old community is claimed by the new community with the largest
 *            member intersection; ties broken by lex(min member id of the overlap).
 *            The winning new community inherits the old id.
 *   Pass 2 — every unclaimed new community keeps a fresh id = its own min-member
 *            node id (lex), exactly as kb_graph_communities mints ids — so a split
 *            (two new communities over one old) gives the best-overlap half the old
 *            id and the other a fresh one, never a collision.
 * old_comm/new_comm are (node,community) assignments from kb_graph_communities.
 * Writes the remapped NEW assignment (one row per new_comm node, node order
 * preserved) into out[] (up to max). Returns rows written, 0 if n_new==0, -1 on a
 * bad argument. Byte-identical regardless of input row order. */
int kb_graph_community_remap(const kb_graph_community_t *old_comm, int n_old,
                             const kb_graph_community_t *new_comm, int n_new,
                             kb_graph_community_t *out, int max);

/* One entry of a structural diff. `kind` classifies the change; `a`/`b` carry the
 * node key(s) or edge endpoints depending on `kind` (see below). */
typedef enum
{
   KB_DIFF_NODE_ADDED = 0,      /* a = node key present in new, absent in old        */
   KB_DIFF_NODE_REMOVED,        /* a = node key present in old, absent in new        */
   KB_DIFF_NODE_RENAMED,        /* a = old key, b = new key (same kind, matched)     */
   KB_DIFF_EDGE_ADDED,          /* a->b (relation in `relation`) new, not in old     */
   KB_DIFF_EDGE_REMOVED,        /* a->b (relation) in old, not in new                */
   KB_DIFF_NEW_ORPHAN,          /* a = node newly degree<=1 (was >1 in old)          */
   KB_DIFF_NEW_CROSS_COMMUNITY, /* a->b edge now crosses a community boundary    */
   KB_DIFF_NEW_CYCLE_MEMBER     /* a = file now in a dependency cycle absent old */
} kb_graph_diff_kind_t;

typedef struct
{
   kb_graph_diff_kind_t kind;
   char a[KB_GRAPH_NODE_MAX];
   char b[KB_GRAPH_NODE_MAX];
   char relation[64]; /* set for edge kinds; empty otherwise */
} kb_graph_diff_entry_t;

/* Compute a deterministic structural diff of two projection generations. `old_*`
 * and `new_*` are the edge arrays (relation-typed) and community assignments of
 * the two generations; new_comm SHOULD already be remapped via
 * kb_graph_community_remap so cross-community findings reflect real coupling, not
 * a renumbered boundary. Emits node/edge add-remove, newly-orphaned nodes, edges
 * that now cross a community boundary, and files newly in a dependency cycle.
 * Every emitted list is total-ordered (kind, then a, then b lex). Writes up to
 * `max` entries into out[]; sets *truncated if capped. Returns entries written, or
 * -1 on a bad argument. Pure; out[] caller-owned. */
int kb_graph_diff(const kb_graph_reledge_t *old_edges, int n_old_edges,
                  const kb_graph_community_t *old_comm, int n_old_comm,
                  const kb_graph_reledge_t *new_edges, int n_new_edges,
                  const kb_graph_community_t *new_comm, int n_new_comm, kb_graph_diff_entry_t *out,
                  int max, int *truncated);

#endif /* KB_GRAPH_ANALYTICS_H */
