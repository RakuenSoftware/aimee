/* kb_store/entity_edges.c: entity-graph storage primitives — Postgres via libpq. */

#include "../headers/aimee.h" /* edge_t */
#include "entity_edges.h"
#include "kb_store_internal.h"
#include "db_postgres.h"
#include "fact_mutation.h"
#include "../headers/rel_types.h" /* correction_behavior + rel_type_is_functional (§4) */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define EE_ERRBUF 256

/* A projected edge is visible only while its owning projection generation and
 * stable project are current. This keeps pending/superseded/detached code out
 * of every generic graph reader without hiding ordinary memory edges. */
#define EE_VISIBLE_PROJECTION                                                                      \
   " AND (COALESCE(edge_origin, '') <> 'code_projection'"                                          \
   " OR EXISTS (SELECT 1 FROM code_projection_generations cpg"                                     \
   " JOIN projects cpp ON cpp.name=cpg.project"                                                    \
   " WHERE cpg.id=entity_edges.projection_generation_id"                                           \
   " AND cpg.state='visible' AND cpp.lifecycle_state='current'))"
/* Recall/fusion reads admit BOTH edge populations: a graph walk cares that two
 * nodes are connected, not which layer connected them. R1-A1 separates the
 * populations in *results* (a typed-recall walk must not return co_discussed and
 * vice-versa) — that is a filter on what gets rendered, not on what may serve as
 * traversal evidence. Co-occurrence rows carry no temporal state; semantic rows
 * do, so admitting them requires the same persistent/promoted lifecycle gate as
 * direct typed recall. In particular, candidates remain quarantined until an
 * operator promotes them. Always pair with EE_VISIBLE_PROJECTION. */
#define EE_ADMIT_CURRENT_SEMANTIC                                                                  \
   " AND (edge_class <> 'semantic'"                                                                \
   " OR (lifecycle_state IN ('persistent','promoted')"                                             \
   " AND superseded_at = '' AND invalidated_at = '' AND suppressed = 0))"
#define EE_ADMIT_CURRENT_SEMANTIC_E                                                                \
   " AND (e.edge_class <> 'semantic'"                                                              \
   " OR (e.lifecycle_state IN ('persistent','promoted')"                                           \
   " AND e.superseded_at = '' AND e.invalidated_at = '' AND e.suppressed = 0))"

#define EE_VISIBLE_PROJECTION_E                                                                    \
   " AND (COALESCE(e.edge_origin, '') <> 'code_projection'"                                        \
   " OR EXISTS (SELECT 1 FROM code_projection_generations cpg"                                     \
   " JOIN projects cpp ON cpp.name=cpg.project"                                                    \
   " WHERE cpg.id=e.projection_generation_id"                                                      \
   " AND cpg.state='visible' AND cpp.lifecycle_state='current'))"

static void edge_row_from_stmt_pg(aimee_pg_stmt_t *st, edge_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   kb_store_copy_text(out->source, sizeof(out->source), aimee_pg_column_text(st, 1));
   kb_store_copy_text(out->relation, sizeof(out->relation), aimee_pg_column_text(st, 2));
   kb_store_copy_text(out->target, sizeof(out->target), aimee_pg_column_text(st, 3));
   out->weight = aimee_pg_column_int(st, 4);
}

/* Cached unique-index state: -1 = unchecked, 0 = absent, 1 = present. */
static int s_unique_index_ready = -1;

int kb_store_entity_edge_upsert(const char *source, const char *relation, const char *target,
                                int64_t window_id, int relation_id, int subject_kind,
                                int object_kind, int *out_added)
{
   if (out_added)
      *out_added = 0;
   if (!source || !relation || !target)
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;

   if (s_unique_index_ready < 0)
      s_unique_index_ready = kb_store_entity_edge_unique_index_exists();

   if (s_unique_index_ready > 0)
   {
      /* Fast path: constraint-backed upsert (Phase 1+). */
      static const char *upsert_sql =
          "INSERT INTO entity_edges (source, relation, target, weight, window_id,"
          " relation_id, subject_kind, object_kind)"
          " VALUES (?1, ?2, ?3, 1, ?4, ?5, ?6, ?7)"
          " ON CONFLICT (source, relation, target)"
          /* Same R1-A1 guard the slow path below carries, and it is needed MORE
           * here: the unique index is on the bare triple, so a semantic row and a
           * co-occurrence row for one triple cannot coexist and the conflict
           * lands on whichever exists. Without this WHERE, a co-occurrence
           * observation bumps a typed fact's weight -- and on a semantic edge
           * weight is a CONFIRMATION COUNT that §5 keys on (promote_durable
           * weight>=threshold, expire_speculative weight<=1), so "these two words
           * appeared together" would count as the user re-asserting the fact.
           * DO UPDATE with a false WHERE degrades to DO NOTHING, which is the
           * right outcome: the co-occurrence observation is dropped rather than
           * corrupting the fact. */
          " DO UPDATE SET weight = entity_edges.weight + 1"
          " WHERE entity_edges.edge_class <> 'semantic'";
      char err[EE_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, upsert_sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", source);
      aimee_pg_bind_text(st, "?2", relation);
      aimee_pg_bind_text(st, "?3", target);
      aimee_pg_bind_int64(st, "?4", window_id);
      aimee_pg_bind_int(st, "?5", relation_id);
      aimee_pg_bind_int(st, "?6", subject_kind);
      aimee_pg_bind_int(st, "?7", object_kind);
      int rc = aimee_pg_step(st, err, sizeof(err));
      int added = aimee_pg_stmt_changes(st);
      aimee_pg_finalize(st);
      if (rc != AIMEE_PG_DONE)
         return -1;
      if (out_added)
         *out_added = (added > 0) ? 1 : 0;
      return 0;
   }

   /* Slow path (pre-migration): probe then insert-or-update. */
   int existed = 0;
   {
      /* Co-occurrence upsert must not find/bump a typed 'semantic' edge that
       * happens to share this triple (R1-A1). */
      static const char *check_sql = "SELECT id FROM entity_edges"
                                     " WHERE source = ?1 AND relation = ?2 AND target = ?3"
                                     " AND edge_class <> 'semantic'";
      char err[EE_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, check_sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", source);
      aimee_pg_bind_text(st, "?2", relation);
      aimee_pg_bind_text(st, "?3", target);
      existed = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
      aimee_pg_finalize(st);
   }

   if (existed)
   {
      static const char *bump_sql = "UPDATE entity_edges SET weight = weight + 1"
                                    " WHERE source = ?1 AND relation = ?2 AND target = ?3"
                                    " AND edge_class <> 'semantic'";
      char err[EE_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, bump_sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", source);
      aimee_pg_bind_text(st, "?2", relation);
      aimee_pg_bind_text(st, "?3", target);
      int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
      aimee_pg_finalize(st);
      return rc;
   }

   static const char *fresh_sql =
       "INSERT INTO entity_edges (source, relation, target, weight, window_id,"
       " relation_id, subject_kind, object_kind)"
       " VALUES (?1, ?2, ?3, 1, ?4, ?5, ?6, ?7)";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, fresh_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", target);
   aimee_pg_bind_int64(st, "?4", window_id);
   aimee_pg_bind_int(st, "?5", relation_id);
   aimee_pg_bind_int(st, "?6", subject_kind);
   aimee_pg_bind_int(st, "?7", object_kind);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE)
      return -1;
   if (out_added)
      *out_added = 1;
   return 0;
}

int kb_store_entity_edge_list_by_entity(const char *entity, edge_t *out, int max)
{
   if (!entity || !out || max <= 0)
      return 0;
   void *conn = kb_store_conn();
   if (!conn)
      return 0;

   /* Co-occurrence recall excludes typed-fact 'semantic' edges (R1-A1 storage
    * boundary): legacy rows default to 'cooccurrence', so <> 'semantic' keeps
    * them and only filters the typed layer out. */
   static const char *sql =
       "SELECT id, source, relation, target, weight FROM entity_edges"
       " WHERE source = ?1 AND edge_class <> 'semantic'" EE_VISIBLE_PROJECTION " UNION ALL"
       " SELECT id, source, relation, target, weight FROM entity_edges"
       " WHERE target = ?2 AND edge_class <> 'semantic'" EE_VISIBLE_PROJECTION " LIMIT ?3";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   aimee_pg_bind_int(st, "?3", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      edge_row_from_stmt_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

static int neighbor_collect(aimee_pg_stmt_t *st, kb_store_entity_neighbor_t *out, int max,
                            char *err, size_t errlen)
{
   int n = 0;
   while (n < max && aimee_pg_step(st, err, errlen) == AIMEE_PG_ROW)
   {
      const char *t = aimee_pg_column_text(st, 0);
      int w = aimee_pg_column_int(st, 1);
      if (t)
      {
         snprintf(out[n].node, sizeof(out[n].node), "%s", t);
         out[n].weight = w;
         n++;
      }
   }
   return n;
}

int kb_store_entity_edge_neighbors(const char *entity, kb_store_entity_neighbor_t *out, int max,
                                   int limit_sql)
{
   if (!entity || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = 50;
   void *conn = kb_store_conn();
   if (!conn)
      return 0;
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "SELECT target, weight FROM entity_edges"
            " WHERE source = ?1" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION " UNION ALL"
            " SELECT source, weight FROM entity_edges"
            " WHERE target = ?2" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION " LIMIT %d",
            limit_sql);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   int n = neighbor_collect(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);
   return n;
}

int kb_store_entity_edge_walk_step(const char *node, edge_t *out, int max)
{
   if (!node || !out || max <= 0)
      return 0;
   void *conn = kb_store_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT id, source, relation, target, weight FROM entity_edges"
       " WHERE (source = ?1 OR target = ?2)" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION
       " ORDER BY weight DESC LIMIT 50";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", node);
   aimee_pg_bind_text(st, "?2", node);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      edge_row_from_stmt_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int kb_store_entity_edge_co_targets(const char *node, const char *relation, int min_weight,
                                    char (*out)[128], int max)
{
   if (!node || !relation || !out || max <= 0)
      return 0;
   void *conn = kb_store_conn();
   if (!conn)
      return 0;
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "SELECT target FROM entity_edges"
            " WHERE source = ?1 AND relation = ?2 AND weight > ?3 AND edge_class <> "
            "'semantic'" EE_VISIBLE_PROJECTION " UNION"
            " SELECT source FROM entity_edges"
            " WHERE target = ?4 AND relation = ?5 AND weight > ?6 AND edge_class <> "
            "'semantic'" EE_VISIBLE_PROJECTION " LIMIT %d",
            max);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", node);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_int(st, "?3", min_weight);
   aimee_pg_bind_text(st, "?4", node);
   aimee_pg_bind_text(st, "?5", relation);
   aimee_pg_bind_int(st, "?6", min_weight);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *t = aimee_pg_column_text(st, 0);
      if (t && t[0])
      {
         snprintf(out[n], sizeof(out[n]), "%s", t);
         n++;
      }
   }
   aimee_pg_finalize(st);
   return n;
}

int kb_store_entity_edge_search_by_token(const char *token, edge_t *out, int max, int limit_sql)
{
   if (!token || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = max;
   void *conn = kb_store_conn();
   if (!conn)
      return 0;
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "SELECT id, source, relation, target, weight FROM entity_edges"
            " WHERE (LOWER(source) = LOWER(?1)"
            "    OR LOWER(target) = LOWER(?2)"
            "    OR LOWER(relation) = LOWER(?3))"
            "   AND edge_class <> 'semantic'" EE_VISIBLE_PROJECTION
            " ORDER BY weight DESC LIMIT %d",
            limit_sql);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", token);
   aimee_pg_bind_text(st, "?2", token);
   aimee_pg_bind_text(st, "?3", token);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      edge_row_from_stmt_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

/* kb_store_relation_schema_list() lived here and SELECTed from
 * memory_relation_schema. Nothing in the tree ever inserts into that table, and
 * memory_ontology_validate() does not consult it, so the function returned zero
 * rows on every deployment and relations.schema_list published an empty list
 * while a different table did the enforcing. The surface now reads the ontology
 * rules directly (memory_ontology_rules), so this reader is gone rather than
 * kept as a second source of truth that can never agree with the first. */

/* --- Phase 1: entity edge uniqueness migration --- */

int kb_store_entity_edge_unique_index_exists(void)
{
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT 1 FROM pg_indexes"
                            " WHERE tablename = 'entity_edges'"
                            "   AND indexname = 'idx_ee_unique_triple'";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   int found = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return found;
}

int kb_store_entity_edge_build_unique_index(int *out_already_exists)
{
   if (out_already_exists)
      *out_already_exists = 0;
   int exists = kb_store_entity_edge_unique_index_exists();
   if (exists < 0)
      return -1;
   if (exists)
   {
      if (out_already_exists)
         *out_already_exists = 1;
      return 0;
   }
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn,
                        "CREATE UNIQUE INDEX IF NOT EXISTS idx_ee_unique_triple"
                        " ON entity_edges (source, relation, target)",
                        err, sizeof(err));
}

/* --- Phase 4: utility-aware graph scoring --- */

/* --- graph explain: rich incident-edge read --- */

int kb_store_entity_edge_explain_by_entity(const char *entity, kb_store_entity_edge_explain_t *out,
                                           int max)
{
   if (!entity || !out || max <= 0)
      return 0;
   void *conn = kb_store_conn();
   if (!conn)
      return 0;
   char sql[1536];
   snprintf(sql, sizeof(sql),
            "SELECT id, source, relation, target, weight,"
            "       COALESCE(structural_weight, 0), COALESCE(utility_score, 0.0),"
            "       COALESCE(edge_origin, '')"
            " FROM entity_edges WHERE (source = ?1 OR target = ?2) AND edge_class <> "
            "'semantic'" EE_VISIBLE_PROJECTION
            " ORDER BY (COALESCE(structural_weight,0) + weight) DESC LIMIT %d",
            max);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      kb_store_copy_text(out[n].source, sizeof(out[n].source), aimee_pg_column_text(st, 1));
      kb_store_copy_text(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 2));
      kb_store_copy_text(out[n].target, sizeof(out[n].target), aimee_pg_column_text(st, 3));
      out[n].weight = aimee_pg_column_int(st, 4);
      out[n].structural_weight = aimee_pg_column_int(st, 5);
      out[n].utility_score = aimee_pg_column_double(st, 6);
      kb_store_copy_text(out[n].edge_origin, sizeof(out[n].edge_origin),
                         aimee_pg_column_text(st, 7));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
