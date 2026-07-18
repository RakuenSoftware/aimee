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
#include "db2/entity_edges.h"
#include "db2/entity_nodes.h"
#include "db2/memory_query.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- Relation gravity table (provisional random-walk priors) --- */

double memory_graph_relation_gravity(const char *relation)
{
   if (!relation || !relation[0])
      return 0.45; /* default ~ co_discussed weight */
   if (strcmp(relation, "defines") == 0)
      return 1.00;
   if (strcmp(relation, "contains") == 0)
      return 0.85;
   if (strcmp(relation, "depends_on") == 0)
      return 0.75;
   if (strcmp(relation, "routes") == 0)
      return 0.70;
   if (strcmp(relation, "exports") == 0)
      return 0.60;
   if (strcmp(relation, "co_edited") == 0)
      return 0.60;
   if (strcmp(relation, "calls") == 0)
      return 0.55;
   if (strcmp(relation, "co_discussed") == 0)
      return 0.45;
   if (strcmp(relation, "imports") == 0)
      return 0.30;
   return 0.45;
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
                               int weight, double effective_utility, int hop)
{
   double gravity = memory_graph_relation_gravity(relation);

   double structural_factor = 1.0;
   if (is_code_edge)
      structural_factor = 1.0 + clampd((double)structural_weight, 0.0, 3.0) / 3.0;

   double observed_factor = 1.0 + clampd(log1p((double)(weight > 0 ? weight : 0)), 0.0, 3.0) / 3.0;

   double utility_factor = 1.0 + clampd(effective_utility, -0.5, 2.0);

   int hop_dist = hop > 1 ? hop : 1;
   double hop_decay = pow(0.5, (double)(hop_dist - 1));

   return gravity * structural_factor * observed_factor * utility_factor * hop_decay;
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

int memory_graph_expand_from_seeds(const char **node_keys, int seed_count, int max_hops,
                                   int max_neighbors, int allow_code_graph,
                                   int utility_scoring_enabled, memory_graph_expansion_t *out,
                                   int max)
{
   if (!node_keys || seed_count <= 0 || !out || max <= 0)
      return 0;
   if (max_hops <= 0)
      max_hops = 2;
   if (max_neighbors <= 0)
      max_neighbors = 16;

   int count = 0;

   for (int s = 0; s < seed_count && count < max; s++)
   {
      const char *seed = node_keys[s];
      if (!seed || !seed[0])
         continue;

      /* Gate: do not enter code subgraphs from a non-code-shaped query. */
      if (!allow_code_graph && node_key_is_code(seed))
         continue;

      /* Seed-direct: memories attached to the seed node itself (the node the
       * vector/lexical hit mapped to) are the most direct evidence. */
      {
         double seed_score = memory_graph_edge_score(NULL, node_key_is_code(seed), 0, 1, 0.0, 1);
         memory_t shits[8];
         int sgot = db2_memory_collect_entity_matches(seed, 4, shits,
                                                      (int)(sizeof(shits) / sizeof(shits[0])));
         for (int h = 0; h < sgot && count < max; h++)
         {
            int dup = 0;
            for (int d = 0; d < count; d++)
               if (out[d].memory_id == shits[h].id)
               {
                  dup = 1;
                  if (seed_score > out[d].graph_score)
                     out[d].graph_score = seed_score;
                  break;
               }
            if (dup)
               continue;
            out[count].memory_id = shits[h].id;
            out[count].graph_score = seed_score;
            out[count].hops = 1;
            snprintf(out[count].via, sizeof(out[count].via), "%s", seed);
            count++;
         }
      }

      db2_entity_edge_weighted_neighbor_t neighbors[64];
      int cap = max_neighbors < 64 ? max_neighbors : 64;
      int n =
          db2_entity_edge_neighbors_weighted(seed, neighbors, cap, cap, utility_scoring_enabled);
      for (int i = 0; i < n && count < max; i++)
      {
         /* Skip code nodes when not allowed. */
         if (!allow_code_graph && node_key_is_code(neighbors[i].node))
            continue;

         /* Hop-1 edge score (relation unknown here → use generic gravity). */
         double escore =
             memory_graph_edge_score(NULL, node_key_is_code(neighbors[i].node), 0,
                                     neighbors[i].weight, neighbors[i].effective_utility, 1);

         /* Look up memories linked to this reached node. */
         memory_t hits[8];
         int got = db2_memory_collect_entity_matches(neighbors[i].node, 4, hits,
                                                     (int)(sizeof(hits) / sizeof(hits[0])));
         for (int h = 0; h < got && count < max; h++)
         {
            /* Dedup. */
            int dup = 0;
            for (int d = 0; d < count; d++)
               if (out[d].memory_id == hits[h].id)
               {
                  dup = 1;
                  if (escore > out[d].graph_score)
                     out[d].graph_score = escore;
                  break;
               }
            if (dup)
               continue;
            out[count].memory_id = hits[h].id;
            out[count].graph_score = escore;
            out[count].hops = 1;
            snprintf(out[count].via, sizeof(out[count].via), "%s", neighbors[i].node);
            count++;
         }
      }
   }
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
   static const char *sql = "SELECT node_key FROM code_embeddings WHERE point_id = ?1";
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
