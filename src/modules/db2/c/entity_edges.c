/* db2/entity_edges.c: entity-graph storage primitives — Postgres via libpq. */

#include "../headers/aimee.h" /* edge_t */
#include "entity_edges.h"
#include "db2_internal.h"
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
   db2_copy_text(out->source, sizeof(out->source), aimee_pg_column_text(st, 1));
   db2_copy_text(out->relation, sizeof(out->relation), aimee_pg_column_text(st, 2));
   db2_copy_text(out->target, sizeof(out->target), aimee_pg_column_text(st, 3));
   out->weight = aimee_pg_column_int(st, 4);
}

/* Cached unique-index state: -1 = unchecked, 0 = absent, 1 = present. */
static int s_unique_index_ready = -1;

int db2_entity_edge_upsert(const char *source, const char *relation, const char *target,
                           int64_t window_id, int relation_id, int subject_kind, int object_kind,
                           int *out_added)
{
   if (out_added)
      *out_added = 0;
   if (!source || !relation || !target)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   if (s_unique_index_ready < 0)
      s_unique_index_ready = db2_entity_edge_unique_index_exists();

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

int db2_entity_edge_list_by_entity(const char *entity, edge_t *out, int max)
{
   if (!entity || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
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

int db2_entity_edge_upsert_semantic(const char *source, const char *relation, const char *target,
                                    int relation_id, int subject_kind, int object_kind,
                                    const char *confidence_class, double confidence, int *out_added)
{
   /* Compatibility entrypoint only.  The authority-aware seam owns every
    * semantic mutation; callers that need user/operator authority must use it
    * directly with a verifier-derived actor. */
   fact_actor_t actor;
   fact_mutation_result_t result;
   fact_assertion_input_t input = {.source = source,
                                   .relation = relation,
                                   .target = target,
                                   .relation_id = relation_id,
                                   .subject_kind = subject_kind,
                                   .object_kind = object_kind,
                                   .confidence_class = confidence_class,
                                   .confidence = confidence,
                                   .assertion_kind = FACT_KIND_WORLD_FACT};
   if (out_added)
      *out_added = 0;
   if (db2_fact_actor_internal(FACT_ACTOR_MODEL, &actor) != 0 ||
       db2_fact_mutation_assert(&actor, &input, &result) != 0)
      return -1;
   if (out_added)
      *out_added = result.changed;
   return 0;
#if 0 /* superseded by fact_mutation; retained temporarily for blame continuity */
   if (out_added)
      *out_added = 0;
   if (!source || !relation || !target)
      return -1;
   if (!confidence_class || !confidence_class[0])
      confidence_class = "C"; /* conservative default (§5) */
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[EE_ERRBUF] = "";

   /* Probe for an existing semantic triple; bump its weight if present (plain
    * probe-then-write so it works whether or not the unique index exists).
    * Correction/supersede semantics for contradictory objects are §4. */
   static const char *probe = "SELECT id FROM entity_edges WHERE source=?1 AND relation=?2 AND"
                              " target=?3 AND edge_class='semantic' LIMIT 1";
   aimee_pg_stmt_t *ps = aimee_pg_prepare(conn, probe, err, sizeof(err));
   if (!ps)
      return -1;
   aimee_pg_bind_text(ps, "?1", source);
   aimee_pg_bind_text(ps, "?2", relation);
   aimee_pg_bind_text(ps, "?3", target);
   int found = (aimee_pg_step(ps, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t existing_id = found ? aimee_pg_column_int64(ps, 0) : 0;
   aimee_pg_finalize(ps);

   if (found)
   {
      /* A re-observation confirms the edge (weight++) and, when the new write has
       * a higher-authority class (A>B>C), upgrades the stored class + confidence;
       * a lower/equal class never downgrades it. Rank is computed in SQL so this
       * stays dialect-portable and avoids a read-modify-write race. Distinct
       * placeholders (no reuse) keep it shim-safe. */
      static const char *bump =
          "UPDATE entity_edges SET weight = weight + 1,"
          " confidence = CASE WHEN ?1 > confidence THEN ?2 ELSE confidence END,"
          " confidence_class = CASE"
          "   WHEN (CASE ?3 WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)"
          "      > (CASE confidence_class WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)"
          "   THEN ?4 ELSE confidence_class END"
          " WHERE id = ?5";
      aimee_pg_stmt_t *u = aimee_pg_prepare(conn, bump, err, sizeof(err));
      if (!u)
         return -1;
      aimee_pg_bind_double(u, "?1", confidence);
      aimee_pg_bind_double(u, "?2", confidence);
      aimee_pg_bind_text(u, "?3", confidence_class);
      aimee_pg_bind_text(u, "?4", confidence_class);
      aimee_pg_bind_int64(u, "?5", existing_id);
      int rc = aimee_pg_step(u, err, sizeof(err));
      aimee_pg_finalize(u);
      return rc == AIMEE_PG_DONE ? 0 : -1;
   }

   /* asserted_at is stamped from the host clock in UTC (not SQL CURRENT_TIMESTAMP,
    * whose timezone differs between Postgres' session zone and the sqlite shim) so
    * it shares one clock with db2_fact_expire_speculative's cutoff — the lexical
    * compare there is then also chronological regardless of DB timezone. */
   time_t now_t = time(NULL);
   struct tm now_tm;
   gmtime_r(&now_t, &now_tm);
   char asserted_at[32];
   strftime(asserted_at, sizeof(asserted_at), "%Y-%m-%d %H:%M:%S", &now_tm);
   /* §4 correction/supersede for contradictory objects. For a FUNCTIONAL
    * (single-valued) relation, a new object contradicts any prior object for the
    * same subject+relation; apply the relation's correction_behavior. The exact
    * triple was already handled (bumped) above, so any prior row here asserts a
    * DIFFERENT object. Multi-valued relations accumulate (no correction). */
   if (rel_type_is_functional(relation))
   {
      const rel_type_def_t *rdef = rel_types_seed_lookup(relation);
      correction_behavior_t corr = rdef ? rdef->correction_behavior : CORR_SUPERSEDE;
      if (corr == CORR_IMMUTABLE)
      {
         static const char *chk =
             "SELECT 1 FROM entity_edges WHERE source=?1 AND relation=?2 AND target<>?3"
             " AND edge_class='semantic' AND superseded_at='' AND suppressed=0 LIMIT 1";
         aimee_pg_stmt_t *cs = aimee_pg_prepare(conn, chk, err, sizeof(err));
         if (cs)
         {
            aimee_pg_bind_text(cs, "?1", source);
            aimee_pg_bind_text(cs, "?2", relation);
            aimee_pg_bind_text(cs, "?3", target);
            int have_prior = (aimee_pg_step(cs, err, sizeof(err)) == AIMEE_PG_ROW);
            aimee_pg_finalize(cs);
            if (have_prior)
               return 0; /* immutable fact is fixed: drop the contradicting new object */
         }
      }
      else
      {
         /* §5 authority guard on the correction itself. Class rank (A>B>C) IS the
          * write's authority: only a direct user assertion earns A. A model-authored
          * (Class B/C) write must therefore not supersede or tombstone a value that
          * outranks it — without this, an ordinary model write silently replaces a
          * user-stated Class-A fact on a single-valued relation, the hole the
          * retraction path already closes with its `confidence_class <> 'A'` guard.
          *
          * Nor may the outranked write be inserted alongside: a functional relation
          * would then hold two contradictory current values and recall would surface
          * the model's next to the user's. It is dropped instead, exactly as the
          * immutable branch drops a contradicting object. */
         static const char *outranked =
             "SELECT 1 FROM entity_edges WHERE source=?1 AND relation=?2 AND target<>?3"
             " AND edge_class='semantic' AND superseded_at='' AND suppressed=0"
             " AND (CASE confidence_class WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)"
             "   > (CASE ?4 WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END) LIMIT 1";
         aimee_pg_stmt_t *os = aimee_pg_prepare(conn, outranked, err, sizeof(err));
         if (!os)
            return -1; /* authority unprovable: never correct blind */
         aimee_pg_bind_text(os, "?1", source);
         aimee_pg_bind_text(os, "?2", relation);
         aimee_pg_bind_text(os, "?3", target);
         aimee_pg_bind_text(os, "?4", confidence_class);
         int outranked_prior = (aimee_pg_step(os, err, sizeof(err)) == AIMEE_PG_ROW);
         aimee_pg_finalize(os);
         if (outranked_prior)
            return 0; /* a higher-authority value stands: drop this write */

         /* The rank test is repeated in the UPDATE so a higher-class row inserted
          * between the probe and here is still not corrected by this write. */
         const char *upd =
             (corr == CORR_HARD_DELETE)
                 ? "UPDATE entity_edges SET suppressed=1, superseded_at=?4 WHERE source=?1"
                   " AND relation=?2 AND target<>?3 AND edge_class='semantic'"
                   " AND superseded_at='' AND suppressed=0"
                   " AND (CASE confidence_class WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)"
                   "   <= (CASE ?5 WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)"
                 : "UPDATE entity_edges SET superseded_at=?4 WHERE source=?1 AND relation=?2"
                   " AND target<>?3 AND edge_class='semantic' AND superseded_at='' AND "
                   "suppressed=0"
                   " AND (CASE confidence_class WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)"
                   "   <= (CASE ?5 WHEN 'A' THEN 3 WHEN 'B' THEN 2 ELSE 1 END)";
         aimee_pg_stmt_t *us = aimee_pg_prepare(conn, upd, err, sizeof(err));
         if (us)
         {
            aimee_pg_bind_text(us, "?1", source);
            aimee_pg_bind_text(us, "?2", relation);
            aimee_pg_bind_text(us, "?3", target);
            aimee_pg_bind_text(us, "?4", asserted_at);
            aimee_pg_bind_text(us, "?5", confidence_class);
            (void)aimee_pg_step(us, err, sizeof(err));
            aimee_pg_finalize(us);
         }
      }
   }

   static const char *ins =
       "INSERT INTO entity_edges (source, relation, target, weight,"
       " relation_id, subject_kind, object_kind, edge_class, confidence_class, confidence,"
       " asserted_at)"
       " VALUES (?1, ?2, ?3, 1, ?4, ?5, ?6, 'semantic', ?7, ?8, ?9)";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, ins, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", target);
   aimee_pg_bind_int(st, "?4", relation_id);
   aimee_pg_bind_int(st, "?5", subject_kind);
   aimee_pg_bind_int(st, "?6", object_kind);
   aimee_pg_bind_text(st, "?7", confidence_class);
   aimee_pg_bind_double(st, "?8", confidence);
   aimee_pg_bind_text(st, "?9", asserted_at);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE)
      return -1;
   if (out_added)
      *out_added = 1;
   return 0;
#endif
}

int db2_entity_edges_semantic_by_entity(const char *entity, edge_t *out, int max)
{
   if (!entity || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id, source, relation, target, weight FROM entity_edges"
                            " WHERE source = ?1 AND edge_class = 'semantic'"
                            " UNION ALL"
                            " SELECT id, source, relation, target, weight FROM entity_edges"
                            " WHERE target = ?2 AND edge_class = 'semantic'"
                            " LIMIT ?3";
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

static int neighbor_collect(aimee_pg_stmt_t *st, db2_entity_neighbor_t *out, int max, char *err,
                            size_t errlen)
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

int db2_entity_edge_neighbors(const char *entity, db2_entity_neighbor_t *out, int max,
                              int limit_sql)
{
   if (!entity || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = 50;
   void *conn = db2_conn();
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

int db2_entity_edge_neighbors_filtered(const char *entity, const char *rel_a, const char *rel_b,
                                       int order_by_weight, db2_entity_neighbor_t *out, int max,
                                       int limit_sql)
{
   if (!entity || !rel_a || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = 20;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[EE_ERRBUF] = "";
   char sql[2048];
   if (rel_b && rel_b[0])
   {
      snprintf(sql, sizeof(sql),
               "SELECT target, weight FROM entity_edges"
               " WHERE source = ?1 AND relation IN (?2, "
               "?3)" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION " UNION ALL"
               " SELECT source, weight FROM entity_edges"
               " WHERE target = ?4 AND relation IN (?5, "
               "?6)" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION "%s LIMIT %d",
               order_by_weight ? " ORDER BY weight DESC" : "", limit_sql);
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_text(st, "?1", entity);
      aimee_pg_bind_text(st, "?2", rel_a);
      aimee_pg_bind_text(st, "?3", rel_b);
      aimee_pg_bind_text(st, "?4", entity);
      aimee_pg_bind_text(st, "?5", rel_a);
      aimee_pg_bind_text(st, "?6", rel_b);
      int n = neighbor_collect(st, out, max, err, sizeof(err));
      aimee_pg_finalize(st);
      return n;
   }

   snprintf(sql, sizeof(sql),
            "SELECT target, weight FROM entity_edges"
            " WHERE source = ?1 AND relation = ?2" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION
            " UNION ALL"
            " SELECT source, weight FROM entity_edges"
            " WHERE target = ?3 AND relation = ?4" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION
            "%s LIMIT %d",
            order_by_weight ? " ORDER BY weight DESC" : "", limit_sql);
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", rel_a);
   aimee_pg_bind_text(st, "?3", entity);
   aimee_pg_bind_text(st, "?4", rel_a);
   int n = neighbor_collect(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_walk_step(const char *node, edge_t *out, int max)
{
   if (!node || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
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

int db2_entity_edge_walk_step_with_kinds(const char *node, db2_entity_edge_with_kinds_t *out,
                                         int max)
{
   if (!node || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT source, relation, target,"
       "       COALESCE(relation_id, 12) AS rid,"
       "       COALESCE(subject_kind, 99) AS sk,"
       "       COALESCE(object_kind, 99) AS ok,"
       "       weight"
       " FROM entity_edges"
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
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].source, sizeof(out[n].source), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].target, sizeof(out[n].target), aimee_pg_column_text(st, 2));
      out[n].relation_id = aimee_pg_column_int(st, 3);
      out[n].subject_kind = aimee_pg_column_int(st, 4);
      out[n].object_kind = aimee_pg_column_int(st, 5);
      out[n].weight = aimee_pg_column_int(st, 6);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_top_targets_by_relation(const char *source, const char *relation,
                                            db2_entity_neighbor_t *out, int max)
{
   if (!source || !relation || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT target, SUM(weight) AS w FROM entity_edges"
            " WHERE LOWER(source) = LOWER(?1) AND relation = ?2 AND edge_class <> "
            "'semantic'" EE_VISIBLE_PROJECTION " GROUP BY target ORDER BY w DESC LIMIT %d",
            max);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   int n = neighbor_collect(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_top_partners_by_relation(const char *entity, const char *relation,
                                             db2_entity_neighbor_t *out, int max)
{
   if (!entity || !relation || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "SELECT partner, SUM(w) AS total FROM ("
            "  SELECT target AS partner, weight AS w FROM entity_edges"
            "  WHERE LOWER(source) = LOWER(?1) AND relation = ?2 AND edge_class <> "
            "'semantic'" EE_VISIBLE_PROJECTION "  UNION ALL"
            "  SELECT source AS partner, weight AS w FROM entity_edges"
            "  WHERE LOWER(target) = LOWER(?3) AND relation = ?4 AND edge_class <> "
            "'semantic'" EE_VISIBLE_PROJECTION
            ") sub GROUP BY partner ORDER BY total DESC LIMIT %d",
            max);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", entity);
   aimee_pg_bind_text(st, "?4", relation);
   int n = neighbor_collect(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_top_distinct_triples(edge_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT DISTINCT 0 AS id, source, relation, target, weight FROM entity_edges"
            " WHERE edge_class <> 'semantic'" EE_VISIBLE_PROJECTION
            " ORDER BY weight DESC LIMIT %d",
            max);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      edge_row_from_stmt_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_co_targets(const char *node, const char *relation, int min_weight,
                               char (*out)[128], int max)
{
   if (!node || !relation || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
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

int db2_entity_edge_bump_utility(const char *key, double delta)
{
   if (!key)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* Clamp to [-5.0, 5.0] and record touch timestamp (Phase 1). */
   static const char *sql =
       "UPDATE entity_edges"
       " SET utility_score = GREATEST(-5.0, LEAST(5.0, utility_score + ?1)),"
       "     utility_touched_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
       " WHERE (source = ?2 OR target = ?3) AND edge_class <> 'semantic'";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_double(st, "?1", delta);
   aimee_pg_bind_text(st, "?2", key);
   aimee_pg_bind_text(st, "?3", key);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_entity_edge_outbound_neighbors(const char *source, db2_entity_neighbor_t *out, int max,
                                       int limit_sql)
{
   if (!source || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = 50;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT target, weight FROM entity_edges"
            " WHERE source = ?1" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION " LIMIT %d",
            limit_sql);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", source);
   int n = neighbor_collect(st, out, max, err, sizeof(err));
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_search_by_token(const char *token, edge_t *out, int max, int limit_sql)
{
   if (!token || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = max;
   void *conn = db2_conn();
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

int db2_entity_edge_prune_orphans(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       /* Orphan pruning is a co-occurrence policy: a co-occurrence edge whose
        * endpoints no longer appear in any prose memory has lost its evidence and
        * is genuinely garbage. A typed fact has NOT — it was asserted directly and
        * carries its own §4/§5 lifecycle (supersede / tombstone, both of which
        * RETAIN the row per "always keep the origin artifact"). Deleting one
        * because nobody happened to write prose about the entity destroys a
        * user-stated Class A fact outright, with no audit trail and no way to
        * distinguish it from one that was never asserted. Semantic rows leave by
        * retraction and expiry only. */
       "DELETE FROM entity_edges WHERE id IN ("
       " SELECT e.id FROM entity_edges e"
       " WHERE COALESCE(e.edge_origin, '') != 'code_projection'"
       " AND e.edge_class <> 'semantic'"
       " AND NOT EXISTS ("
       "  SELECT 1 FROM memories m WHERE m.tier IN ('L1','L2')"
       "  AND (m.key LIKE '%' || e.source || '%' OR m.content LIKE '%' || e.source || '%')"
       " )"
       " AND NOT EXISTS ("
       "  SELECT 1 FROM memories m WHERE m.tier IN ('L1','L2')"
       "  AND (m.key LIKE '%' || e.target || '%' OR m.content LIKE '%' || e.target || '%')"
       " )"
       ")";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : 0;
}

int db2_entity_edge_normalize_weights(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Rescaling is a CO-OCCURRENCE concern. There, weight is an observation
    * tally whose absolute size means nothing, so normalising it per relation
    * makes edges comparable. On a semantic edge weight is a confirmation count
    * with meaning, and §5 reads it as one: promote_durable fires at
    * weight >= threshold, expire_speculative at weight <= 1. Rescaling breaks
    * both. A Class B fact asserted ONCE, sharing a relation with an edge of
    * weight 5, is rescaled to 20 and promoted to durable on the next cycle --
    * durability earned by an unrelated edge's name. And once nothing sits at
    * weight 1 any more, Class C speculation stops expiring entirely. Those are
    * the exact two outcomes §5 exists to prevent, so semantic rows are left
    * alone. */
   static const char *sql = "UPDATE entity_edges SET weight = "
                            " CAST(weight * 100.0 / "
                            "  (SELECT MAX(weight) FROM entity_edges e2"
                            "   WHERE e2.relation = entity_edges.relation"
                            "     AND e2.edge_class <> 'semantic')"
                            " AS INTEGER)"
                            " WHERE weight > 0"
                            " AND edge_class <> 'semantic'"
                            " AND COALESCE(edge_origin, '') != 'code_projection'"
                            " AND (SELECT MAX(weight) FROM entity_edges e2"
                            "      WHERE e2.relation = entity_edges.relation"
                            "        AND e2.edge_class <> 'semantic') > 1"
                            /* Skip rows that are already at their normalized value.
                             * Without this the pass rewrites every edge to the value
                             * it already holds on each run — measured: 2 of 2 rows on
                             * a converged graph. Harmless when normalize was dead
                             * code, but it now runs on EVERY maintenance cycle, so an
                             * idle graph would burn WAL and bump updated_at forever. */
                            " AND weight <> CAST(weight * 100.0 /"
                            "  (SELECT MAX(weight) FROM entity_edges e2"
                            "   WHERE e2.relation = entity_edges.relation"
                            "     AND e2.edge_class <> 'semantic') AS INTEGER)";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : 0;
}

int db2_relation_schema_list(db2_relation_schema_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT relation_id, subject_kind, object_kind FROM memory_relation_schema"
       " ORDER BY relation_id, subject_kind, object_kind";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].relation_id = aimee_pg_column_int(st, 0);
      out[n].subject_kind = aimee_pg_column_int(st, 1);
      out[n].object_kind = aimee_pg_column_int(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* --- Phase 1: entity edge uniqueness migration --- */

int db2_entity_edge_unique_index_exists(void)
{
   void *conn = db2_conn();
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

int db2_entity_edge_dedup_audit(db2_entity_edge_dedup_report_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT COUNT(*) total,"
                            "       SUM(CASE WHEN cnt > 1 THEN 1 ELSE 0 END) dup_triples,"
                            "       SUM(CASE WHEN cnt > 1 THEN cnt - 1 ELSE 0 END) dup_rows,"
                            "       MAX(cnt) largest_group"
                            " FROM (SELECT COUNT(*) cnt FROM entity_edges"
                            "       GROUP BY source, relation, target) g";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->total_rows = aimee_pg_column_int64(st, 0);
      out->dup_triples = aimee_pg_column_int64(st, 1);
      out->dup_rows = aimee_pg_column_int64(st, 2);
      out->largest_group = aimee_pg_column_int64(st, 3);
   }
   aimee_pg_finalize(st);
   static const char *size_sql =
       "SELECT COALESCE(pg_total_relation_size('entity_edges') / 1024, 0)";
   st = aimee_pg_prepare(conn, size_sql, err, sizeof(err));
   if (st)
   {
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         out->table_size_kb = aimee_pg_column_int64(st, 0);
      aimee_pg_finalize(st);
   }
   return 0;
}

int db2_entity_edge_dedup_migrate(const char *rollback_path, int dry_run,
                                  db2_entity_edge_dedup_report_t *out)
{
   db2_entity_edge_dedup_report_t report;
   if (db2_entity_edge_dedup_audit(&report) < 0)
      return -1;
   if (out)
      *out = report;
   if (dry_run || report.dup_rows == 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (rollback_path && rollback_path[0])
   {
      static const char *export_sql =
          "SELECT source, relation, target, COUNT(*) cnt,"
          "       SUM(weight) total_weight,"
          "       MAX(utility_score) max_utility,"
          "       MAX(utility_touched_at) newest_touched"
          " FROM entity_edges GROUP BY source, relation, target HAVING COUNT(*) > 1";
      char err[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, export_sql, err, sizeof(err));
      if (st)
      {
         FILE *fp = fopen(rollback_path, "w");
         if (fp)
         {
            while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
            {
               const char *src = aimee_pg_column_text(st, 0);
               const char *rel = aimee_pg_column_text(st, 1);
               const char *tgt = aimee_pg_column_text(st, 2);
               const char *touched = aimee_pg_column_text(st, 6);
               fprintf(fp,
                       "{\"source\":\"%s\",\"relation\":\"%s\",\"target\":\"%s\","
                       "\"cnt\":%lld,\"total_weight\":%lld,"
                       "\"max_utility\":%.6g,\"newest_touched\":\"%s\"}\n",
                       src ? src : "", rel ? rel : "", tgt ? tgt : "",
                       (long long)aimee_pg_column_int64(st, 3),
                       (long long)aimee_pg_column_int64(st, 4), aimee_pg_column_double(st, 5),
                       touched ? touched : "");
            }
            fclose(fp);
         }
         aimee_pg_finalize(st);
      }
   }
   char err[256] = "";
   static const char *merge_sql =
       "UPDATE entity_edges e"
       " SET weight = g.total_weight,"
       "     utility_score = GREATEST(-5.0, LEAST(5.0, g.max_utility)),"
       "     utility_touched_at = g.newest_touched"
       " FROM (SELECT MIN(id) AS survivor_id, source, relation, target,"
       "              SUM(weight) AS total_weight, MAX(utility_score) AS max_utility,"
       "              MAX(utility_touched_at) AS newest_touched"
       "       FROM entity_edges WHERE edge_class <> 'semantic'"
       "       GROUP BY source, relation, target HAVING COUNT(*) > 1) g"
       " WHERE e.id = g.survivor_id";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, merge_sql, err, sizeof(err));
   if (!st)
      return -1;
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE)
      return -1;
   static const char *delete_sql =
       "DELETE FROM entity_edges WHERE id IN"
       " (SELECT id FROM (SELECT id,"
       "  ROW_NUMBER() OVER (PARTITION BY source, relation, target ORDER BY id) AS rn"
       "  FROM entity_edges WHERE edge_class <> 'semantic') t WHERE t.rn > 1)";
   st = aimee_pg_prepare(conn, delete_sql, err, sizeof(err));
   if (!st)
      return -1;
   rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_entity_edge_build_unique_index(int *out_already_exists)
{
   if (out_already_exists)
      *out_already_exists = 0;
   int exists = db2_entity_edge_unique_index_exists();
   if (exists < 0)
      return -1;
   if (exists)
   {
      if (out_already_exists)
         *out_already_exists = 1;
      return 0;
   }
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn,
                        "CREATE UNIQUE INDEX IF NOT EXISTS idx_ee_unique_triple"
                        " ON entity_edges (source, relation, target)",
                        err, sizeof(err));
}

/* --- Phase 4: utility-aware graph scoring --- */

double db2_entity_edge_utility_decay(double raw_score, const char *touched_at, int half_life_days)
{
   if (raw_score == 0.0)
      return 0.0;
   if (!touched_at || !touched_at[0])
   {
      /* Legacy sentinel: non-zero score with no timestamp → keep raw score. */
      return raw_score;
   }
   /* Parse touched_at as "YYYY-MM-DD HH:MM:SS" (UTC). */
   int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
   if (sscanf(touched_at, "%d-%d-%d %d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) < 6)
      return raw_score; /* unparseable → keep raw */
   /* 1970-01-01 is the sentinel for "no useful timestamp" — return 0. */
   if (yr <= 1970 || mo < 1 || mo > 12 || dy < 1 || dy > 31)
      return 0.0;

   /* Compute elapsed days from touch timestamp to now. */
   time_t now = time(NULL);
   struct tm t;
   memset(&t, 0, sizeof(t));
   t.tm_year = yr - 1900;
   t.tm_mon = mo - 1;
   t.tm_mday = dy;
   t.tm_hour = hr;
   t.tm_min = mn;
   t.tm_sec = sc;
   time_t touch_epoch = mktime(&t);
   if (touch_epoch == (time_t)-1)
      return raw_score;

   double elapsed_days = difftime(now, touch_epoch) / 86400.0;
   if (elapsed_days < 0.0)
      elapsed_days = 0.0;

   int hl = (half_life_days > 0) ? half_life_days : 90;
   /* lambda = ln(2) / half_life_days */
   double lambda = 0.693147 / (double)hl;
   double decayed = raw_score * exp(-lambda * elapsed_days);
   /* Clamp to [-5.0, 5.0] (same as bump bounds). */
   if (decayed > 5.0)
      decayed = 5.0;
   if (decayed < -5.0)
      decayed = -5.0;
   return decayed;
}

double db2_entity_edge_prune_priority(int weight, double decayed_utility, double utility_weight)
{
   return (double)weight + utility_weight * decayed_utility;
}

/* Split an edge_class/confidence_class row pair into the scoring fields the
 * traversal callers need. Co-occurrence rows leave confidence_class empty so a
 * caller can tell "no confidence signal" from a genuine class C. */
static void edge_class_fields(const char *edge_class, const char *confidence_class,
                              int *out_is_semantic, char *out_class, size_t class_cap)
{
   int semantic = (edge_class && strcmp(edge_class, "semantic") == 0);
   if (out_is_semantic)
      *out_is_semantic = semantic;
   if (!out_class || class_cap == 0)
      return;
   out_class[0] = '\0';
   if (semantic && confidence_class && confidence_class[0])
      db2_copy_text(out_class, class_cap, confidence_class);
}

int db2_entity_edge_neighbors_weighted(const char *entity, db2_entity_edge_weighted_neighbor_t *out,
                                       int max, int limit_sql, int utility_scoring_enabled)
{
   if (!entity || !out || max <= 0)
      return 0;
   if (limit_sql <= 0)
      limit_sql = 50;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[2048];
   snprintf(sql, sizeof(sql),
            "SELECT target, weight, utility_score, utility_touched_at, relation,"
            "       edge_class, confidence_class"
            " FROM entity_edges WHERE source = ?1" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION
            " UNION ALL"
            " SELECT source, weight, utility_score, utility_touched_at, relation,"
            "       edge_class, confidence_class"
            " FROM entity_edges WHERE target = ?2" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION
            " LIMIT %d",
            limit_sql);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *node = aimee_pg_column_text(st, 0);
      if (!node)
         continue;
      snprintf(out[n].node, sizeof(out[n].node), "%s", node);
      out[n].weight = aimee_pg_column_int(st, 1);
      out[n].utility_score = aimee_pg_column_double(st, 2);
      db2_copy_text(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 4));
      edge_class_fields(aimee_pg_column_text(st, 5), aimee_pg_column_text(st, 6),
                        &out[n].is_semantic, out[n].confidence_class,
                        sizeof(out[n].confidence_class));
      if (utility_scoring_enabled)
      {
         const char *ts = aimee_pg_column_text(st, 3);
         out[n].effective_utility = db2_entity_edge_utility_decay(out[n].utility_score, ts, 90);
      }
      else
      {
         out[n].effective_utility = 0.0;
      }
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_two_hop_neighbors(const char *entity, int max, int limit_per_hop,
                                      db2_entity_edge_hop_t *out)
{
   if (!entity || !out || max <= 0)
      return 0;
   if (limit_per_hop <= 0)
      limit_per_hop = 32;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* CTE: hop1 = 1-hop neighbours; final = hop2 neighbours not in hop1.
    *
    * Each UNION branch is PARENTHESISED because it carries its own LIMIT.
    * Postgres rejects `SELECT ... LIMIT n UNION ALL SELECT ...` as a syntax
    * error: an unparenthesised LIMIT binds to the whole union, so the grammar
    * will not accept another branch after it. SQLite accepts the same text.
    * This function's only tests run against the sqlite shim, and it had no
    * production caller, so it was accepted by the suite and had in fact never
    * executed against the real database. Verified against PostgreSQL 17. */
   char sql[4096];
   snprintf(sql, sizeof(sql),
            "WITH hop1 AS ("
            "  (SELECT target AS node, weight, relation, edge_class, confidence_class"
            "   FROM entity_edges"
            "   WHERE source = ?1" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION " LIMIT %d)"
            "  UNION ALL"
            "  (SELECT source AS node, weight, relation, edge_class, confidence_class"
            "   FROM entity_edges"
            "   WHERE target = ?2" EE_ADMIT_CURRENT_SEMANTIC EE_VISIBLE_PROJECTION " LIMIT %d)"
            ")"
            " SELECT node, weight, 1 AS hop, relation, edge_class, confidence_class FROM hop1"
            " UNION ALL"
            " SELECT DISTINCT e.target, e.weight, 2 AS hop, e.relation, e.edge_class,"
            "        e.confidence_class"
            " FROM entity_edges e"
            " JOIN hop1 h ON (e.source = h.node)"
            " WHERE e.target != ?3" EE_ADMIT_CURRENT_SEMANTIC_E EE_VISIBLE_PROJECTION_E
            "   AND e.target NOT IN (SELECT node FROM hop1)"
            " LIMIT %d",
            limit_per_hop, limit_per_hop, max);
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   aimee_pg_bind_text(st, "?3", entity);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *node = aimee_pg_column_text(st, 0);
      if (!node)
         continue;
      snprintf(out[n].node, sizeof(out[n].node), "%s", node);
      out[n].weight = aimee_pg_column_int(st, 1);
      out[n].hop = aimee_pg_column_int(st, 2);
      db2_copy_text(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 3));
      edge_class_fields(aimee_pg_column_text(st, 4), aimee_pg_column_text(st, 5),
                        &out[n].is_semantic, out[n].confidence_class,
                        sizeof(out[n].confidence_class));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_edge_backfill_utility_touched_at(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Update rows with non-zero utility but empty/1970 utility_touched_at. */
   static const char *sql =
       "UPDATE entity_edges"
       " SET utility_touched_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
       " WHERE utility_score != 0.0"
       "   AND edge_class <> 'semantic'"
       "   AND (utility_touched_at = '' OR utility_touched_at = '1970-01-01 00:00:00')";
   char err[EE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : 0;
}

/* --- graph explain: rich incident-edge read --- */

int db2_entity_edge_explain_by_entity(const char *entity, db2_entity_edge_explain_t *out, int max)
{
   if (!entity || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
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
      db2_copy_text(out[n].source, sizeof(out[n].source), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].target, sizeof(out[n].target), aimee_pg_column_text(st, 3));
      out[n].weight = aimee_pg_column_int(st, 4);
      out[n].structural_weight = aimee_pg_column_int(st, 5);
      out[n].utility_score = aimee_pg_column_double(st, 6);
      db2_copy_text(out[n].edge_origin, sizeof(out[n].edge_origin), aimee_pg_column_text(st, 7));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
