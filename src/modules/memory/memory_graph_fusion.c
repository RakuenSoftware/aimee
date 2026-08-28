/* src/memory_graph_fusion.c: Phase 6 graph-vector fusion rerank.
 *
 * Implements the proposal's four-stage retrieval fusion scoring:
 *   relation gravity table, edge-score formula, code-shape detection,
 *   and seed-driven graph expansion mapped back to memory candidates.
 *
 * All scoring is gated behind allow_code_graph (set by query-shape detection)
 * and the runtime utility_scoring_enabled flag; default behaviour is unchanged. */

#include "aimee.h"
#include "memory.h"
#include "memory_graph_fusion.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/entity_nodes.h"
#include "modules/db2/c/memory_query.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "log.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Relation gravity table (provisional random-walk priors) --- */

/* Table lookup proper: returns 1 and fills *out when `relation` has an explicit
 * prior, 0 when it does not. Kept separate from the public accessor so callers
 * that know the edge's class can pick a class-appropriate fallback instead of
 * the co-occurrence default. */
static int relation_gravity_lookup(const char *relation, double *out)
{
   if (!relation || !relation[0])
      return 0;
   double g;
   if (strcmp(relation, "defines") == 0)
      g = 1.00;
   else if (strcmp(relation, "contains") == 0)
      g = 0.85;
   else if (strcmp(relation, "depends_on") == 0)
      g = 0.75;
   else if (strcmp(relation, "routes") == 0)
      g = 0.70;
   else if (strcmp(relation, "exports") == 0)
      g = 0.60;
   else if (strcmp(relation, "co_edited") == 0)
      g = 0.60;
   else if (strcmp(relation, "calls") == 0)
      g = 0.55;
   else if (strcmp(relation, "co_discussed") == 0)
      g = 0.45;
   else if (strcmp(relation, "imports") == 0)
      g = 0.30;
   else
      return 0;
   if (out)
      *out = g;
   return 1;
}

double memory_graph_relation_gravity(const char *relation)
{
   double g;
   if (relation_gravity_lookup(relation, &g))
      return g;
   return MEMORY_GRAPH_GRAVITY_DEFAULT; /* default ~ co_discussed weight */
}

double memory_graph_confidence_factor(const char *confidence_class)
{
   /* Only semantic edges carry a class; an empty/absent one means "no confidence
    * signal" (a co-occurrence edge) and must not be penalised for it. */
   if (!confidence_class || !confidence_class[0])
      return 1.0;
   if (confidence_class[0] == 'A' || confidence_class[0] == 'a')
      return 1.00;
   if (confidence_class[0] == 'B' || confidence_class[0] == 'b')
      return 0.75;
   /* Class C, and anything unrecognised: fail conservative, matching the
    * FACT_CLASS default the writer applies for an unspecified class. */
   return 0.50;
}

static double clampd(double v, double lo, double hi)
{
   if (v < lo)
      return lo;
   if (v > hi)
      return hi;
   return v;
}

double memory_graph_edge_score(const char *relation, int is_code_edge, int structural_weight,
                               int weight, double effective_utility, int hop,
                               const char *confidence_class)
{
   /* A non-empty confidence_class is what marks this as a typed-fact edge. */
   int is_semantic = (confidence_class && confidence_class[0]);

   double gravity;
   if (!relation_gravity_lookup(relation, &gravity))
      gravity = is_semantic ? MEMORY_GRAPH_GRAVITY_SEMANTIC : MEMORY_GRAPH_GRAVITY_DEFAULT;

   double confidence_factor = memory_graph_confidence_factor(confidence_class);

   double structural_factor = 1.0;
   if (is_code_edge)
      structural_factor = 1.0 + clampd((double)structural_weight, 0.0, 3.0) / 3.0;

   double observed_factor = 1.0 + clampd(log1p((double)(weight > 0 ? weight : 0)), 0.0, 3.0) / 3.0;

   double utility_factor = 1.0 + clampd(effective_utility, -0.5, 2.0);

   int hop_dist = hop > 1 ? hop : 1;
   double hop_decay = pow(0.5, (double)(hop_dist - 1));

   return gravity * confidence_factor * structural_factor * observed_factor * utility_factor *
          hop_decay;
}

/* --- Code-shape detection --- */

/* Returns 1 if the token looks code-shaped: contains a file extension,
 * path separator, line ref, or symbol syntax. */
static int token_is_code_shaped(const char *q)
{
   if (!q || !q[0])
      return 0;

   /* Path separator with a file-ish segment. */
   if (strchr(q, '/'))
   {
      /* Heuristic: a slash plus a dot suggests a path with extension. */
      if (strchr(q, '.'))
         return 1;
   }

   /* Symbol syntax: "::" or "->". */
   if (strstr(q, "::") || strstr(q, "->"))
      return 1;

   /* Known code file extensions. */
   static const char *exts[] = {".c",  ".h",  ".cc", ".cpp",  ".hpp", ".inc", ".py",  ".js",
                                ".ts", ".go", ".rs", ".java", ".rb",  ".sh",  ".sql", NULL};
   size_t len = strlen(q);
   for (int i = 0; exts[i]; i++)
   {
      size_t el = strlen(exts[i]);
      if (len >= el && strcmp(q + len - el, exts[i]) == 0)
         return 1;
   }

   /* Line reference like "foo.c:123". */
   const char *colon = strchr(q, ':');
   if (colon && isdigit((unsigned char)colon[1]))
      return 1;

   return 0;
}

code_seed_reason_t memory_graph_detect_code_shape(const char *query, int caller_flag,
                                                  memory_query_plan_t *plan)
{
   code_seed_reason_t reason = CODE_SEED_NONE;

   /* Rule 1: explicit caller flag wins. */
   if (caller_flag)
   {
      reason = CODE_SEED_CALLER;
   }
   else if (query && query[0])
   {
      /* Rule 2: token heuristics — check each whitespace token. */
      char buf[1024];
      snprintf(buf, sizeof(buf), "%s", query);
      char *saveptr = NULL;
      char *tok = strtok_r(buf, " \t\n", &saveptr);
      while (tok)
      {
         if (token_is_code_shaped(tok))
         {
            reason = CODE_SEED_TOKEN;
            break;
         }
         tok = strtok_r(NULL, " \t\n", &saveptr);
      }
   }

   if (plan)
   {
      plan->code_seed_reason = reason;
      plan->allow_code_graph = (reason != CODE_SEED_NONE) ? 1 : 0;
   }
   return reason;
}

/* --- Graph expansion from seeds --- */

/* Returns 1 if a canonical node key denotes a code node (file/symbol/etc). */
static int node_key_is_code(const char *key)
{
   if (!key)
      return 0;
   return strncmp(key, "file:", 5) == 0 || strncmp(key, "symbol:", 7) == 0 ||
          strncmp(key, "import:", 7) == 0 || strncmp(key, "export:", 7) == 0 ||
          strncmp(key, "route:", 6) == 0 || strncmp(key, "project:", 8) == 0;
}

/* BFS visit budget across the whole seed set. The traversal is breadth-first over
 * ALL seeds at once rather than per-seed, so a node's recorded hop distance is its
 * minimum over every seed, and the total number of neighbour reads is bounded by
 * this constant instead of by seed_count * fan-out^max_hops. */
#define GRAPH_EXPAND_MAX_NODES 192

/* Rows a single batched level read may return: a full chunk of the frontier,
 * each node contributing up to the per-node neighbour cap. */
#define GRAPH_BATCH_NEIGHBORS (EE_FRONTIER_BATCH_MAX * 8)

typedef struct
{
   char key[GRAPH_ENDPOINT_MAX];
} graph_visit_t;

/* 1 iff `key` is already in the visit queue. Linear scan: the queue is bounded at
 * GRAPH_EXPAND_MAX_NODES, so this stays cheap next to the per-node DB reads. */
static int graph_visit_seen(const graph_visit_t *queue, int n, const char *key)
{
   for (int i = 0; i < n; i++)
      if (strcmp(queue[i].key, key) == 0)
         return 1;
   return 0;
}

/* Record the memories attached to `node`, reached at 1-based `hop` with `score`.
 * Dedups on memory id across the whole result set, keeping the strongest score and
 * the shortest hop distance seen (a node reachable at hop 1 from one seed and hop 2
 * from another is a hop-1 result). Returns the new count. */
static int graph_record_node(const char *node, double score, int hop, memory_graph_expansion_t *out,
                             int count, int max)
{
   memory_t hits[8];
   int got =
       db2_memory_collect_entity_matches(node, 4, hits, (int)(sizeof(hits) / sizeof(hits[0])));
   for (int h = 0; h < got && count < max; h++)
   {
      int dup = 0;
      for (int d = 0; d < count; d++)
         if (out[d].memory_id == hits[h].id)
         {
            dup = 1;
            if (score > out[d].graph_score)
               out[d].graph_score = score;
            if (hop < out[d].hops)
            {
               out[d].hops = hop;
               snprintf(out[d].via, sizeof(out[d].via), "%s", node);
            }
            break;
         }
      if (dup)
         continue;
      out[count].memory_id = hits[h].id;
      out[count].graph_score = score;
      out[count].hops = hop;
      snprintf(out[count].via, sizeof(out[count].via), "%s", node);
      count++;
   }
   return count;
}

/* Graph expansion needs the relational store. Where that store is unreachable --
 * on aimee-server, which links no libpq and reaches PostgreSQL only through the
 * kb client socket -- every db2_* helper below guards internally and returns
 * nothing, so expansion yields zero results and the caller cannot tell that from
 * "this memory genuinely has no neighbours".
 *
 * A silent zero is the worst of the three possible behaviours: a crash would be
 * found immediately and a real answer would be correct, but silence looks like a
 * working feature with an empty graph. Say it once per process, the same way
 * db1_client/git_ownership.c reports an unreachable store: enough to tell a store
 * that is down from one that is quiet, without a line per call.
 *
 * This is a diagnostic, not a fix. Graph memory works on aimee-kb and does not on
 * aimee-server until the store migration puts PostgreSQL on both -- see
 * docs/proposals/pending/one-store-postgres-and-pgvectorscale-everywhere.md.
 */
static void graph_warn_store_unreachable(void)
{
   static int warned;
   if (warned)
      return;
   warned = 1;
   LOG_WARN("memory.graph", "graph expansion is unavailable: the relational store is unreachable "
                            "from this process, so neighbour expansion returns no results "
                            "(vector and lexical retrieval are unaffected)");
}

int memory_graph_expand_from_seeds(const char **node_keys, int seed_count, int max_hops,
                                   int max_neighbors, int allow_code_graph,
                                   int utility_scoring_enabled, memory_graph_expansion_t *out,
                                   int max)
{
   if (!node_keys || seed_count <= 0 || !out || max <= 0)
      return 0;
   /* Probe once, before doing any work: without the store every helper below
      returns empty and the result is indistinguishable from a memory with no
      neighbours. */
   if (!db2_conn())
   {
      graph_warn_store_unreachable();
      return 0;
   }
   if (max_hops <= 0)
      max_hops = 2;
   if (max_neighbors <= 0)
      max_neighbors = 16;

   /* Heap, not automatic: the queue is ~96 KiB and memory retrieval is a deep call
    * chain that has exhausted the thread stack on buffers of this size before. */
   graph_visit_t *queue = calloc(GRAPH_EXPAND_MAX_NODES, sizeof(*queue));
   if (!queue)
      return 0;
   int n_queue = 0;

   /* Heap for the same reason as the queue: a batched level returns up to
    * GRAPH_BATCH_NEIGHBORS rows, which is far too large to sit on this call
    * chain's stack. */
   db2_entity_edge_weighted_neighbor_t *neighbors =
       calloc(GRAPH_BATCH_NEIGHBORS, sizeof(*neighbors));
   if (!neighbors)
   {
      free(queue);
      return 0;
   }

   int count = 0;

   /* Level 0: the seeds themselves. Memories attached to a seed node (the node the
    * vector/lexical hit mapped to) are the most direct evidence and score at hop 1,
    * i.e. undecayed. */
   for (int s = 0; s < seed_count && count < max; s++)
   {
      const char *seed = node_keys[s];
      if (!seed || !seed[0])
         continue;

      /* Gate: do not enter code subgraphs from a non-code-shaped query. */
      if (!allow_code_graph && node_key_is_code(seed))
         continue;

      if (graph_visit_seen(queue, n_queue, seed))
         continue;
      if (n_queue < GRAPH_EXPAND_MAX_NODES)
      {
         snprintf(queue[n_queue].key, sizeof(queue[n_queue].key), "%s", seed);
         n_queue++;
      }

      /* The seed is a node, not an edge: no relation, no confidence class. */
      double seed_score = memory_graph_edge_score(NULL, node_key_is_code(seed), 0, 1, 0.0, 1, NULL);
      count = graph_record_node(seed, seed_score, 1, out, count, max);
   }

   /* Levels 1..max_hops. `queue` doubles as the visited set and the BFS queue:
    * [level_start, level_end) is the current frontier, and neighbours discovered
    * while expanding it are appended, forming the next frontier. */
   int level_start = 0;
   int level_end = n_queue;
   int cap = max_neighbors < 64 ? max_neighbors : 64;

   for (int hop = 1; hop <= max_hops && count < max; hop++)
   {
      /* One statement per LEVEL, not per node. The frontier is read in chunks of
       * at most EE_FRONTIER_BATCH_MAX so the generated SQL stays bounded; within
       * a chunk each node still contributes at most `cap` neighbours, so the
       * traversal visits what it visited before at a fraction of the round
       * trips. This is what makes a two-hop walk cost two reads rather than one
       * per node on the first ring. */
      for (int chunk = level_start; chunk < level_end && count < max;
           chunk += EE_FRONTIER_BATCH_MAX)
      {
         const char *frontier[EE_FRONTIER_BATCH_MAX];
         int fn = 0;
         for (int qi = chunk; qi < level_end && fn < EE_FRONTIER_BATCH_MAX; qi++)
            frontier[fn++] = queue[qi].key;

         int n = db2_entity_edge_neighbors_weighted_batch(
             frontier, fn, neighbors, GRAPH_BATCH_NEIGHBORS, cap, utility_scoring_enabled);
         for (int i = 0; i < n && count < max; i++)
         {
            if (!neighbors[i].node[0])
               continue;

            /* Skip code nodes when not allowed. */
            if (!allow_code_graph && node_key_is_code(neighbors[i].node))
               continue;

            /* Already reached at this hop or a shorter one — its memories are
             * recorded and re-expanding it would only repeat work. */
            if (graph_visit_seen(queue, n_queue, neighbors[i].node))
               continue;

            /* Enqueue for the next level. A full queue stops the traversal from
             * growing but does not stop this node's memories being recorded. */
            if (n_queue < GRAPH_EXPAND_MAX_NODES)
            {
               snprintf(queue[n_queue].key, sizeof(queue[n_queue].key), "%s", neighbors[i].node);
               n_queue++;
            }

            /* Edge score at this hop. The reader returns the traversed edge's
             * relation and, for typed facts, its confidence class, so both the
             * gravity table and the A/B/C weighting apply instead of the generic
             * default. hop feeds the pow(0.5, hop-1) decay, so the second ring is
             * worth half the first. */
            double escore = memory_graph_edge_score(
                neighbors[i].relation, node_key_is_code(neighbors[i].node), 0, neighbors[i].weight,
                neighbors[i].effective_utility, hop, neighbors[i].confidence_class);

            count = graph_record_node(neighbors[i].node, escore, hop, out, count, max);
         }
      }

      /* Advance to the frontier just appended; stop early if it is empty. */
      level_start = level_end;
      level_end = n_queue;
      if (level_start >= level_end)
         break;
   }

   free(neighbors);
   free(queue);
   return count;
}

int memory_graph_point_id_to_node_key(int64_t point_id, char *node_key, size_t cap)
{
   if (!node_key || cap == 0)
      return -1;
   node_key[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT ce.node_key FROM code_embeddings ce"
                            " JOIN projects p ON p.name=ce.project WHERE ce.point_id=?1"
                            " AND p.lifecycle_state='current'"
                            " AND ce.generation=p.current_generation";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", point_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v && v[0])
      {
         snprintf(node_key, cap, "%s", v);
         rc = 0;
      }
   }
   aimee_pg_finalize(st);
   return rc;
}

void memory_graph_populate_score_parts(memory_score_parts_t *parts, int64_t memory_id,
                                       const memory_graph_expansion_t *expansions,
                                       int expansion_count)
{
   if (!parts || !expansions)
      return;
   for (int i = 0; i < expansion_count; i++)
   {
      if (expansions[i].memory_id != memory_id)
         continue;
      parts->graph_score = expansions[i].graph_score;
      if (node_key_is_code(expansions[i].via))
         parts->code_proximity = expansions[i].graph_score;
      break;
   }
}

/* --- Phase 7: path-credit feedback distribution --- */

int memory_graph_distribute_path_credit(double delta, const memory_graph_path_edge_t *edges,
                                        int path_length, double *out_credits)
{
   if (!edges || !out_credits || path_length <= 0)
      return -1;

   /* Compute each edge's raw weight = gravity(relation) * hop_decay. */
   double denom = 0.0;
   for (int i = 0; i < path_length; i++)
   {
      int hop = edges[i].hop > 1 ? edges[i].hop : 1;
      double hop_decay = pow(0.5, (double)(hop - 1));
      double w = memory_graph_relation_gravity(edges[i].relation) * hop_decay;
      out_credits[i] = w; /* stash raw weight; rescale below */
      denom += w;
   }

   if (denom <= 0.0)
      return -1;

   /* Normalise so the per-edge credits sum to delta. */
   for (int i = 0; i < path_length; i++)
      out_credits[i] = delta * out_credits[i] / denom;

   return 0;
}

/* --- Recall-path fusion state (thread-local, set per request) --- */

static __thread int g_fusion_on = 0;
static __thread const memory_graph_expansion_t *g_fusion_exp = NULL;
static __thread int g_fusion_exp_n = 0;
static __thread int g_fusion_utility_scoring = 1; /* ablation sub-gates, default on */
static __thread int g_fusion_code_projection = 1;

void memory_fusion_state_set(const char *graph_code_fusion_state)
{
   /* "on" runs the fusion rerank and returns the fused order. "shadow" and
    * "off" leave the returned order unchanged (shadow-mode delta capture is a
    * follow-up); both are treated as not-on here. */
   g_fusion_on = (graph_code_fusion_state && strcmp(graph_code_fusion_state, "on") == 0) ? 1 : 0;
}

void memory_fusion_gates_set(int utility_scoring, int code_projection)
{
   g_fusion_utility_scoring = utility_scoring ? 1 : 0;
   g_fusion_code_projection = code_projection ? 1 : 0;
}

int memory_fusion_utility_scoring(void)
{
   return g_fusion_utility_scoring;
}

int memory_fusion_code_projection(void)
{
   return g_fusion_code_projection;
}

int memory_fusion_state_is_on(void)
{
   return g_fusion_on;
}

void memory_fusion_state_clear(void)
{
   g_fusion_on = 0;
   g_fusion_exp = NULL;
   g_fusion_exp_n = 0;
   g_fusion_utility_scoring = 1;
   g_fusion_code_projection = 1;
}

void memory_fusion_expansions_set(const memory_graph_expansion_t *expansions, int count)
{
   g_fusion_exp = expansions;
   g_fusion_exp_n = (expansions && count > 0) ? count : 0;
}

void memory_fusion_expansions_apply(memory_score_parts_t *parts, int64_t memory_id)
{
   if (g_fusion_exp && g_fusion_exp_n > 0)
      memory_graph_populate_score_parts(parts, memory_id, g_fusion_exp, g_fusion_exp_n);
}

void memory_fusion_expansions_clear(void)
{
   g_fusion_exp = NULL;
   g_fusion_exp_n = 0;
}
