/* memory_graph.c: entity-relationship graph extraction + query + graph-
 * based retrieval boost + pruning + weight normalization.  All SQL is
 * routed through db2/entity_edges.{c,h}; this file owns the algorithms
 * (BFS, decay, key-token tokenisation, scoring). */
#include "aimee.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/entity_edges.h"
#include "db2/memory_query.h"
#include "entity_edges.h"
#endif
#include "log.h"
#include "memory.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(AIMEE_DB2_DISABLED)

/* Server profile: entity graph storage is DB2-owned by aimee-kb. */
int memory_extract_edges(int64_t window_id, char **file_refs, int file_count, char **terms,
                         int term_count)
{
   (void)window_id;
   (void)file_refs;
   (void)file_count;
   (void)terms;
   (void)term_count;
   return 0;
}

int memory_query_edges(const char *entity, edge_t *out, int max)
{
   (void)entity;
   (void)out;
   (void)max;
   return 0;
}

void memory_graph_boost(char **query_terms, int term_count, boost_map_t *out)
{
   (void)query_terms;
   (void)term_count;
   if (out)
      out->count = 0;
}

int memory_graph_related(char **seed_keys, int seed_count, graph_related_t *out, int max)
{
   (void)seed_keys;
   (void)seed_count;
   (void)out;
   (void)max;
   return 0;
}

int memory_graph_prune(void)
{
   return 0;
}

int memory_graph_normalize(void)
{
   return 0;
}

#else

/* --- Entity Edge Extraction --- */

int memory_extract_edges(int64_t window_id, char **file_refs, int file_count, char **terms,
                         int term_count)
{
   if (window_id <= 0)
      return 0;

   int edges_added = 0;

   /* Co-edited pairs: files in the same window. */
   for (int i = 0; i < file_count; i++)
   {
      for (int j = i + 1; j < file_count; j++)
      {
         if (!file_refs[i] || !file_refs[j])
            continue;
         int added = 0;
         if (db2_entity_edge_upsert(file_refs[i], "co_edited", file_refs[j], window_id,
                                    (int)REL_CO_EDITED, (int)NODE_FILE, (int)NODE_FILE,
                                    &added) == 0 &&
             added)
            edges_added++;
      }
   }

   /* Co-discussed pairs: terms together in the same window.  Limit
    * terms to 10 to avoid combinatorial explosion. */
   int max_terms = term_count < 10 ? term_count : 10;
   for (int i = 0; i < max_terms; i++)
   {
      for (int j = i + 1; j < max_terms; j++)
      {
         if (!terms[i] || !terms[j])
            continue;
         if (strlen(terms[i]) < 3 || strlen(terms[j]) < 3)
            continue;
         int added = 0;
         if (db2_entity_edge_upsert(terms[i], "co_discussed", terms[j], window_id,
                                    (int)REL_CO_DISCUSSED, (int)NODE_CONCEPT, (int)NODE_CONCEPT,
                                    &added) == 0 &&
             added)
            edges_added++;
      }
   }

   return edges_added;
}

/* --- Query edges --- */

int memory_query_edges(const char *entity, edge_t *out, int max)
{
   return db2_entity_edge_list_by_entity(entity, out, max);
}

/* --- Graph boost for search ---
 *
 * Traverse edges up to `depth` hops from query_terms, decay weight by
 * 1/(hop+1).  Returns a boost map as entity names + scores stored in
 * parallel arrays. */

static void boost_map_add(boost_map_t *map, const char *entity, double score)
{
   for (int i = 0; i < map->count; i++)
   {
      if (strcmp(map->entries[i].entity, entity) == 0)
      {
         if (score > map->entries[i].score)
            map->entries[i].score = score;
         return;
      }
   }
   if (map->count >= MAX_BOOST_ENTRIES)
      return;
   snprintf(map->entries[map->count].entity, sizeof(map->entries[0].entity), "%s", entity);
   map->entries[map->count].score = score;
   map->count++;
}

static void graph_boost(char **query_terms, int term_count, int depth, boost_map_t *result)
{
   result->count = 0;

   /* Hop 0: direct neighbors of query terms. */
   for (int t = 0; t < term_count; t++)
   {
      db2_entity_neighbor_t buf[64];
      int n = db2_entity_edge_neighbors(query_terms[t], buf, 64, 50);
      for (int i = 0; i < n; i++)
         boost_map_add(result, buf[i].node, (double)buf[i].weight);
   }

   /* Hop 1+: traverse from discovered neighbors. */
   if (depth >= 2 && result->count > 0)
   {
      int hop0_count = result->count;
      for (int i = 0; i < hop0_count && i < 50; i++)
      {
         db2_entity_neighbor_t buf[64];
         int n = db2_entity_edge_neighbors(result->entries[i].entity, buf, 64, 50);
         for (int k = 0; k < n; k++)
            boost_map_add(result, buf[k].node, (double)buf[k].weight / 2.0);
      }
   }
}

/* Public wrapper: compute graph boost map from query terms (BFS to 4 hops). */
void memory_graph_boost(char **query_terms, int term_count, boost_map_t *out)
{
   graph_boost(query_terms, term_count, 4, out);
}

/* --- Graph-powered related memory retrieval ---
 *
 * Given seed memory keys, walk co_discussed edges (1-hop) plus
 * co_edited / depends_on bridges (hop 2), then look up memories whose
 * key/content contains the related terms.  Returns count up to max. */
int memory_graph_related(char **seed_keys, int seed_count, graph_related_t *out, int max)
{
   if (!seed_keys || seed_count <= 0 || !out || max <= 0)
      return 0;

   /* Collect related terms via 1-hop co_discussed edges from seed keys. */
   boost_map_t related;
   related.count = 0;

   for (int s = 0; s < seed_count && s < 16; s++)
   {
      if (!seed_keys[s])
         continue;
      char copy[512];
      snprintf(copy, sizeof(copy), "%s", seed_keys[s]);
      for (char *p = copy; *p; p++)
         *p = tolower((unsigned char)*p);
      char *saveptr = NULL;
      char *tok = strtok_r(copy, " \t\n_-/.", &saveptr);
      while (tok)
      {
         if (strlen(tok) >= 3)
         {
            db2_entity_neighbor_t buf[20];
            int n = db2_entity_edge_neighbors_filtered(tok, "co_discussed", NULL,
                                                       /* order_by_weight = */ 1, buf, 20, 20);
            for (int i = 0; i < n; i++)
               boost_map_add(&related, buf[i].node, (double)buf[i].weight);
         }
         tok = strtok_r(NULL, " \t\n_-/.", &saveptr);
      }
   }

   /* Hop 2: expand through co_edited / depends_on from current frontier. */
   {
      int hop1_count = related.count;
      int expanded = 0;
      for (int i = 0; i < hop1_count && expanded < 12; i++, expanded++)
      {
         const char *src = related.entries[i].entity;
         if (!src || !src[0])
            continue;
         db2_entity_neighbor_t buf[15];
         int n = db2_entity_edge_neighbors_filtered(src, "co_edited", "depends_on",
                                                    /* order_by_weight = */ 1, buf, 15, 15);
         for (int k = 0; k < n; k++)
            boost_map_add(&related, buf[k].node, (double)buf[k].weight * 0.5);
      }
   }

   if (related.count == 0)
      return 0;

   /* Look up memories whose key/content contain any of the related terms. */
   int count = 0;
   for (int r = 0; r < related.count && count < max; r++)
   {
      char pattern[256];
      snprintf(pattern, sizeof(pattern), "%%%s%%", related.entries[r].entity);

      db2_memory_search_match_t hits[10];
      int n = db2_memory_search_by_pattern(pattern, hits, (int)(sizeof(hits) / sizeof(hits[0])));
      for (int i = 0; i < n && count < max; i++)
      {
         int is_seed = 0;
         for (int s = 0; s < seed_count; s++)
         {
            if (seed_keys[s] && hits[i].key[0] && strstr(hits[i].key, seed_keys[s]))
            {
               is_seed = 1;
               break;
            }
         }
         if (is_seed)
            continue;

         int dup = 0;
         for (int d = 0; d < count; d++)
         {
            if (out[d].id == hits[i].id)
            {
               dup = 1;
               break;
            }
         }
         if (dup)
            continue;

         out[count].id = hits[i].id;
         snprintf(out[count].key, sizeof(out[count].key), "%s", hits[i].key);
         snprintf(out[count].content, sizeof(out[count].content), "%s", hits[i].content);
         out[count].score = related.entries[r].score;
         count++;
      }
   }

   return count;
}

int memory_graph_prune(void)
{
   return db2_entity_edge_prune_orphans();
}

int memory_graph_normalize(void)
{
   return db2_entity_edge_normalize_weights();
}

#endif
