/* src/db2/code_projection.c: code-index graph projection ledger. */

#include "code_projection.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "entity_nodes.h"
#include "memory_ontology.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_ERRBUF 256

/* Structural trust defaults per relation (from proposal). */
static int structural_weight_for_relation(const char *relation)
{
   if (!relation)
      return 1;
   if (strcmp(relation, "defines") == 0)
      return 3;
   if (strcmp(relation, "contains") == 0)
      return 2;
   if (strcmp(relation, "exports") == 0)
      return 2;
   if (strcmp(relation, "routes") == 0)
      return 2;
   if (strcmp(relation, "depends_on") == 0)
      return 2;
   if (strcmp(relation, "calls") == 0)
      return 1;
   if (strcmp(relation, "imports") == 0)
      return 1;
   return 1;
}

/* --- Generation lifecycle --- */

int64_t db2_code_projection_generation_create(const char *project)
{
   if (!project || !*project)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* Stamp the aimee build that produced this generation (graph-feedback S2), so
    * the snapshot-diff route can refuse to compare across extractor versions. */
   static const char *sql =
       "INSERT INTO code_projection_generations (project, state, started_at,"
       " extractor_version, pipeline_version)"
       " VALUES (?1, 'pending', to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'), ?2, ?2)"
       " RETURNING id";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", AIMEE_VERSION);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_code_projection_generation_publish(int64_t gen_id, const char *project)
{
   if (gen_id <= 0 || !project || !*project)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CP_ERRBUF] = "";

   /* Supersede the current visible generation (if any). */
   static const char *supersede_sql = "UPDATE code_projection_generations"
                                      " SET state = 'superseded'"
                                      " WHERE project = ?1 AND state = 'visible'";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, supersede_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);

   /* Flip our generation from pending to visible. */
   static const char *publish_sql =
       "UPDATE code_projection_generations"
       " SET state = 'visible',"
       "     visible_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
       " WHERE id = ?1 AND state = 'pending'";
   st = aimee_pg_prepare(conn, publish_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE)
      return -1;

   /* Stamp projection_generation_id on entity_edges for this generation. */
   static const char *stamp_sql = "UPDATE entity_edges e"
                                  " SET projection_generation_id = ?1"
                                  " FROM code_projection_edges cpe"
                                  " WHERE cpe.generation_id = ?2"
                                  "   AND e.source = cpe.source"
                                  "   AND e.relation = cpe.relation"
                                  "   AND e.target = cpe.target"
                                  "   AND e.edge_origin = 'code_projection'";
   st = aimee_pg_prepare(conn, stamp_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_int64(st, "?2", gen_id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

int db2_code_projection_generation_abort(int64_t gen_id, const char *error_msg)
{
   if (gen_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "UPDATE code_projection_generations"
                            " SET state = 'aborted',"
                            "     aborted_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'),"
                            "     error = ?2"
                            " WHERE id = ?1 AND state = 'pending'";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_text(st, "?2", error_msg ? error_msg : "");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int64_t db2_code_projection_visible_id(const char *project)
{
   if (!project || !*project)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT id FROM code_projection_generations"
                            " WHERE project = ?1 AND state = 'visible'";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_code_projection_list_edges(const char *project, code_projection_edge_t *out, int max)
{
   if (!project || !*project || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* Edges of the project's currently-visible generation. The JOIN to
    * code_projection_generations on state='visible' guarantees we read the
    * published graph, never a pending/superseded one. */
   static const char *sql = "SELECT cpe.source, cpe.relation, cpe.target"
                            " FROM code_projection_edges cpe"
                            " JOIN code_projection_generations g ON g.id = cpe.generation_id"
                            " WHERE cpe.project = ?1 AND g.state = 'visible'"
                            " ORDER BY cpe.source, cpe.target"
                            " LIMIT ?2";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_int64(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *src = aimee_pg_column_text(st, 0);
      const char *rel = aimee_pg_column_text(st, 1);
      const char *tgt = aimee_pg_column_text(st, 2);
      snprintf(out[n].source, sizeof(out[n].source), "%s", src ? src : "");
      snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel ? rel : "");
      snprintf(out[n].target, sizeof(out[n].target), "%s", tgt ? tgt : "");
      out[n].structural_weight = structural_weight_for_relation(rel);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_code_projection_list_edges_for_gen(int64_t gen_id, code_projection_edge_t *out, int max)
{
   if (gen_id <= 0 || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* Total order (source, target, relation) so that if the LIMIT boundary cuts
    * through same-(source,target) edges the truncation is deterministic — the
    * derived community partition can't depend on DB row ordering. */
   static const char *sql = "SELECT source, relation, target"
                            " FROM code_projection_edges"
                            " WHERE generation_id = ?1"
                            " ORDER BY source, target, relation"
                            " LIMIT ?2";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_int64(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *src = aimee_pg_column_text(st, 0);
      const char *rel = aimee_pg_column_text(st, 1);
      const char *tgt = aimee_pg_column_text(st, 2);
      snprintf(out[n].source, sizeof(out[n].source), "%s", src ? src : "");
      snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel ? rel : "");
      snprintf(out[n].target, sizeof(out[n].target), "%s", tgt ? tgt : "");
      out[n].structural_weight = structural_weight_for_relation(rel);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* --- Community membership (graph-feedback S-community) --- */

int db2_code_projection_communities_replace(int64_t gen_id, const char *project,
                                            const code_projection_community_t *rows, int n)
{
   if (gen_id <= 0 || n < 0 || (n > 0 && !rows))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CP_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   /* Clear any prior partition for this generation (recompute is idempotent). */
   static const char *del_sql = "DELETE FROM code_projection_communities WHERE generation_id = ?1";
   aimee_pg_stmt_t *del = aimee_pg_prepare(conn, del_sql, err, sizeof(err));
   if (!del)
      goto rollback;
   aimee_pg_bind_int64(del, "?1", gen_id);
   if (aimee_pg_step(del, err, sizeof(err)) != AIMEE_PG_DONE)
   {
      aimee_pg_finalize(del);
      goto rollback;
   }
   aimee_pg_finalize(del);

   if (n > 0)
   {
      static const char *ins_sql =
          "INSERT INTO code_projection_communities"
          " (generation_id, project, node_id, community_id) VALUES (?1, ?2, ?3, ?4)";
      aimee_pg_stmt_t *ins = aimee_pg_prepare(conn, ins_sql, err, sizeof(err));
      if (!ins)
         goto rollback;
      for (int i = 0; i < n; i++)
      {
         aimee_pg_reset(ins);
         aimee_pg_bind_int64(ins, "?1", gen_id);
         aimee_pg_bind_text(ins, "?2", project ? project : "");
         aimee_pg_bind_text(ins, "?3", rows[i].node_id);
         aimee_pg_bind_text(ins, "?4", rows[i].community_id);
         if (aimee_pg_step(ins, err, sizeof(err)) != AIMEE_PG_DONE)
         {
            aimee_pg_finalize(ins);
            goto rollback;
         }
      }
      aimee_pg_finalize(ins);
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      goto rollback;
   return 0;

rollback:
   /* Best-effort: if COMMIT itself failed after the server already committed, this
    * ROLLBACK is a no-op and the rows persist. Acceptable here — the caller treats
    * community membership as a derived analytic and recomputes it idempotently on
    * the next publish; a later slice needing atomic cross-generation swaps would
    * revisit this. aimee_pg_in_transaction avoids a spurious "no transaction". */
   if (aimee_pg_in_transaction(conn))
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return -1;
}

int db2_code_projection_communities_list(int64_t gen_id, code_projection_community_t *out, int max)
{
   if (gen_id <= 0 || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT node_id, community_id"
                            " FROM code_projection_communities"
                            " WHERE generation_id = ?1"
                            " ORDER BY node_id"
                            " LIMIT ?2";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_int64(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *node = aimee_pg_column_text(st, 0);
      const char *comm = aimee_pg_column_text(st, 1);
      snprintf(out[n].node_id, sizeof(out[n].node_id), "%s", node ? node : "");
      snprintf(out[n].community_id, sizeof(out[n].community_id), "%s", comm ? comm : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_code_projection_generation_meta(int64_t gen_id, code_projection_generation_meta_t *out)
{
   if (gen_id <= 0 || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "SELECT id, project, state, source_hash, extractor_version, pipeline_version"
       " FROM code_projection_generations WHERE id = ?1";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   if (rc != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return rc == AIMEE_PG_DONE ? 1 : -1; /* 1 = no such generation */
   }
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   const char *p = aimee_pg_column_text(st, 1);
   const char *s = aimee_pg_column_text(st, 2);
   const char *sh = aimee_pg_column_text(st, 3);
   const char *ev = aimee_pg_column_text(st, 4);
   const char *pv = aimee_pg_column_text(st, 5);
   snprintf(out->project, sizeof(out->project), "%s", p ? p : "");
   snprintf(out->state, sizeof(out->state), "%s", s ? s : "");
   snprintf(out->source_hash, sizeof(out->source_hash), "%s", sh ? sh : "");
   snprintf(out->extractor_version, sizeof(out->extractor_version), "%s", ev ? ev : "");
   snprintf(out->pipeline_version, sizeof(out->pipeline_version), "%s", pv ? pv : "");
   aimee_pg_finalize(st);
   return 0;
}

int db2_code_projection_generations_list(const char *project, code_projection_generation_row_t *out,
                                         int max)
{
   if (!project || !*project || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT id, state, started_at FROM code_projection_generations"
                            " WHERE project = ?1 ORDER BY id DESC LIMIT ?2";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_int64(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      const char *s = aimee_pg_column_text(st, 1);
      const char *t = aimee_pg_column_text(st, 2);
      snprintf(out[n].state, sizeof(out[n].state), "%s", s ? s : "");
      snprintf(out[n].started_at, sizeof(out[n].started_at), "%s", t ? t : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_code_projection_project_fingerprint(const char *project, char *out, size_t out_len)
{
   if (!project || !*project || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* md5 over (path, content-hash) of every file in the project, ordered — two
    * scans with identical file contents produce the same fingerprint, so the
    * drain can skip an unchanged project (content-addressed idempotency). An empty
    * project hashes md5('') deterministically (built once, then skipped). */
   static const char *sql =
       "SELECT md5(coalesce(string_agg(f.path || ':' || f.hash, E'\\n' ORDER BY f.path), ''))"
       " FROM files f JOIN projects p ON f.project_id = p.id WHERE p.name = ?1";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *h = aimee_pg_column_text(st, 0);
      if (h && h[0])
      {
         snprintf(out, out_len, "%s", h);
         rc = 0;
      }
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_code_projection_generation_set_source_hash(int64_t gen_id, const char *source_hash)
{
   if (gen_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "UPDATE code_projection_generations SET source_hash = ?2 WHERE id = ?1";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_text(st, "?2", source_hash ? source_hash : "");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_code_projection_visible_source_hash(const char *project, char *out, size_t out_len)
{
   if (!project || !*project || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT source_hash FROM code_projection_generations"
                            " WHERE project = ?1 AND state = 'visible'";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if ((aimee_pg_bind_text(st, "?1", project), aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      const char *h = aimee_pg_column_text(st, 0);
      if (h)
         snprintf(out, out_len, "%s", h);
   }
   aimee_pg_finalize(st);
   return 0; /* out == "" when there is no visible generation yet */
}

int db2_code_projection_generation_update_counts(int64_t gen_id, int64_t edge_count,
                                                 int64_t node_count)
{
   if (gen_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "UPDATE code_projection_generations"
                            " SET edge_count = ?2, node_count = ?3"
                            " WHERE id = ?1";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_int64(st, "?2", edge_count);
   aimee_pg_bind_int64(st, "?3", node_count);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_code_projection_cleanup_old(const char *project, int min_days_old)
{
   if (!project || !*project || min_days_old < 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[512];
   snprintf(sql, sizeof(sql),
            "DELETE FROM code_projection_generations"
            " WHERE project = ?1"
            "   AND state IN ('superseded','aborted')"
            "   AND started_at < to_char("
            "       CURRENT_TIMESTAMP - INTERVAL '%d days',"
            "       'YYYY-MM-DD HH24:MI:SS')",
            min_days_old);
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : 0;
}

/* --- Edge ledger --- */

int db2_code_projection_edge_record(int64_t gen_id, const char *project, const char *source,
                                    const char *relation, const char *target,
                                    const char *source_hash)
{
   if (gen_id <= 0 || !source || !relation || !target)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "INSERT INTO code_projection_edges"
                            " (generation_id, project, source, relation, target, source_hash)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6)"
                            " ON CONFLICT DO NOTHING";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", gen_id);
   aimee_pg_bind_text(st, "?2", project ? project : "");
   aimee_pg_bind_text(st, "?3", source);
   aimee_pg_bind_text(st, "?4", relation);
   aimee_pg_bind_text(st, "?5", target);
   aimee_pg_bind_text(st, "?6", source_hash ? source_hash : "");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_code_projection_edge_upsert(int64_t gen_id, const char *project, const char *source,
                                    const char *relation, const char *target, int relation_id,
                                    int subject_kind, int object_kind, int structural_weight)
{
   if (!source || !relation || !target)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int sw = (structural_weight > 0) ? structural_weight : structural_weight_for_relation(relation);
   /* Insert new code edge with weight=0 (no observed evidence yet).
    * On conflict preserve observed weight/utility/utility_touched_at;
    * update structural metadata and generation id. */
   static const char *sql = "INSERT INTO entity_edges"
                            " (source, relation, target, weight, window_id,"
                            "  relation_id, subject_kind, object_kind,"
                            "  edge_origin, structural_weight, structural_updated_at,"
                            "  projection_generation_id)"
                            " VALUES (?1, ?2, ?3, 0, 0, ?4, ?5, ?6,"
                            "         'code_projection', ?7,"
                            "         to_char(CURRENT_TIMESTAMP,'YYYY-MM-DD HH24:MI:SS'), ?8)"
                            " ON CONFLICT (source, relation, target) DO UPDATE SET"
                            "  relation_id = excluded.relation_id,"
                            "  subject_kind = excluded.subject_kind,"
                            "  object_kind = excluded.object_kind,"
                            "  edge_origin = excluded.edge_origin,"
                            "  structural_weight = excluded.structural_weight,"
                            "  structural_updated_at = excluded.structural_updated_at,"
                            "  projection_generation_id = excluded.projection_generation_id,"
                            "  weight = entity_edges.weight,"
                            "  utility_score = entity_edges.utility_score,"
                            "  utility_touched_at = entity_edges.utility_touched_at";
   char err[CP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", relation);
   aimee_pg_bind_text(st, "?3", target);
   aimee_pg_bind_int(st, "?4", relation_id);
   aimee_pg_bind_int(st, "?5", subject_kind);
   aimee_pg_bind_int(st, "?6", object_kind);
   aimee_pg_bind_int(st, "?7", sw);
   aimee_pg_bind_int64(st, "?8", gen_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

/* --- Project sync helpers --- */

/* Helper: upsert one edge into entity_edges + record in ledger. */
static int project_edge(int64_t gen_id, const char *project, const char *source,
                        const char *relation, const char *target, int rel_id, int subject_kind,
                        int object_kind)
{
   if (db2_code_projection_edge_upsert(gen_id, project, source, relation, target, rel_id,
                                       subject_kind, object_kind, 0) != 0)
      return -1;
   return db2_code_projection_edge_record(gen_id, project, source, relation, target, "");
}

int64_t db2_code_projection_sync_project(const char *project, int64_t gen_id)
{
   if (!project || !*project || gen_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   int64_t edge_count = 0;
   char err[CP_ERRBUF] = "";

   /* Fetch project id. */
   static const char *proj_sql = "SELECT id FROM projects WHERE name = ?1 LIMIT 1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, proj_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   int64_t proj_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      proj_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (proj_id < 0)
      return -1; /* project not indexed yet */

   /* Build project node key. */
   char proj_key[GRAPH_ENDPOINT_MAX];
   if (db2_entity_node_key_project(project, proj_key, sizeof(proj_key)) != 0)
      return -1;

   /* --- Iterate files: emit contains + per-file edges --- */
   static const char *files_sql = "SELECT id, path FROM files WHERE project_id = ?1 ORDER BY id";
   st = aimee_pg_prepare(conn, files_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", proj_id);

   /* Collect file rows into a buffer (avoid nested queries on same conn). */
#define MAX_FILES 4096
   typedef struct
   {
      int64_t id;
      char path[512];
   } file_row_t;
   file_row_t *files = NULL;
   int file_count = 0;
   {
      /* Count first. */
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         file_count++;
      aimee_pg_finalize(st);
      if (file_count > MAX_FILES)
         file_count = MAX_FILES;
      files = (file_row_t *)malloc((size_t)file_count * sizeof(file_row_t));
      if (!files)
         return -1;
      st = aimee_pg_prepare(conn, files_sql, err, sizeof(err));
      if (!st)
      {
         free(files);
         return -1;
      }
      aimee_pg_bind_int64(st, "?1", proj_id);
      int i = 0;
      while (i < file_count && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         files[i].id = aimee_pg_column_int64(st, 0);
         const char *p = aimee_pg_column_text(st, 1);
         snprintf(files[i].path, sizeof(files[i].path), "%s", p ? p : "");
         i++;
      }
      file_count = i;
      aimee_pg_finalize(st);
   }

   for (int fi = 0; fi < file_count; fi++)
   {
      char file_key[GRAPH_ENDPOINT_MAX];
      if (db2_entity_node_key_file(project, files[fi].path, file_key, sizeof(file_key)) != 0)
         continue;

      /* contains: project → file */
      if (project_edge(gen_id, project, proj_key, "contains", file_key, REL_DEPENDS_ON, NODE_MODULE,
                       NODE_FILE) == 0)
         edge_count++;

      /* defines: file → symbol (from terms where kind='definition') */
      {
         static const char *defs_sql =
             "SELECT name, kind FROM terms WHERE file_id = ?1 AND kind IN "
             "('definition','function','method','class','struct','type','variable')";
         aimee_pg_stmt_t *ds = aimee_pg_prepare(conn, defs_sql, err, sizeof(err));
         if (ds)
         {
            aimee_pg_bind_int64(ds, "?1", files[fi].id);
            while (aimee_pg_step(ds, err, sizeof(err)) == AIMEE_PG_ROW)
            {
               const char *sym_name = aimee_pg_column_text(ds, 0);
               const char *kind = aimee_pg_column_text(ds, 1);
               if (!sym_name)
                  continue;
               char sym_key[GRAPH_ENDPOINT_MAX];
               if (db2_entity_node_key_symbol(project, sym_name, sym_key, sizeof(sym_key)) != 0)
                  continue;
               int nkind = NODE_OTHER;
               if (kind)
               {
                  if (strcmp(kind, "function") == 0 || strcmp(kind, "method") == 0)
                     nkind = NODE_FUNCTION;
                  else if (strcmp(kind, "struct") == 0 || strcmp(kind, "class") == 0 ||
                           strcmp(kind, "type") == 0)
                     nkind = NODE_STRUCT;
               }
               if (project_edge(gen_id, project, file_key, "defines", sym_key, REL_CALLS, NODE_FILE,
                                nkind) == 0)
                  edge_count++;
            }
            aimee_pg_finalize(ds);
         }
      }

      /* exports: file → export:proj:name */
      {
         static const char *exp_sql = "SELECT name FROM file_exports WHERE file_id = ?1";
         aimee_pg_stmt_t *es = aimee_pg_prepare(conn, exp_sql, err, sizeof(err));
         if (es)
         {
            aimee_pg_bind_int64(es, "?1", files[fi].id);
            while (aimee_pg_step(es, err, sizeof(err)) == AIMEE_PG_ROW)
            {
               const char *name = aimee_pg_column_text(es, 0);
               if (!name)
                  continue;
               char exp_key[GRAPH_ENDPOINT_MAX];
               char enc_name[GRAPH_ENDPOINT_MAX];
               db2_entity_node_encode_component(name, enc_name, sizeof(enc_name));
               char combined[GRAPH_ENDPOINT_MAX * 2];
               snprintf(combined, sizeof(combined), "%s:%s", project,
                        enc_name); /* simplified key */
               char enc_proj[GRAPH_ENDPOINT_MAX];
               db2_entity_node_encode_component(project, enc_proj, sizeof(enc_proj));
               snprintf(exp_key, sizeof(exp_key), "export:%s:%s", enc_proj, enc_name);
               if (strlen(exp_key) >= GRAPH_ENDPOINT_MAX)
                  continue;
               if (project_edge(gen_id, project, file_key, "exports", exp_key, REL_IMPLEMENTS,
                                NODE_FILE, NODE_OTHER) == 0)
                  edge_count++;
            }
            aimee_pg_finalize(es);
         }
      }

      /* imports: file → import:proj:name */
      {
         static const char *imp_sql = "SELECT name FROM file_imports WHERE file_id = ?1";
         aimee_pg_stmt_t *is = aimee_pg_prepare(conn, imp_sql, err, sizeof(err));
         if (is)
         {
            aimee_pg_bind_int64(is, "?1", files[fi].id);
            while (aimee_pg_step(is, err, sizeof(err)) == AIMEE_PG_ROW)
            {
               const char *name = aimee_pg_column_text(is, 0);
               if (!name)
                  continue;
               char enc_proj[GRAPH_ENDPOINT_MAX], enc_name[GRAPH_ENDPOINT_MAX];
               db2_entity_node_encode_component(project, enc_proj, sizeof(enc_proj));
               db2_entity_node_encode_component(name, enc_name, sizeof(enc_name));
               char imp_key[GRAPH_ENDPOINT_MAX];
               snprintf(imp_key, sizeof(imp_key), "import:%s:%s", enc_proj, enc_name);
               if (strlen(imp_key) >= GRAPH_ENDPOINT_MAX)
                  continue;
               if (project_edge(gen_id, project, file_key, "imports", imp_key, REL_DEPENDS_ON,
                                NODE_FILE, NODE_OTHER) == 0)
                  edge_count++;
            }
            aimee_pg_finalize(is);
         }
      }

      /* routes: file → route:proj:name (terms where kind='route') */
      {
         static const char *route_sql =
             "SELECT name FROM terms WHERE file_id = ?1 AND kind = 'route'";
         aimee_pg_stmt_t *rs = aimee_pg_prepare(conn, route_sql, err, sizeof(err));
         if (rs)
         {
            aimee_pg_bind_int64(rs, "?1", files[fi].id);
            while (aimee_pg_step(rs, err, sizeof(err)) == AIMEE_PG_ROW)
            {
               const char *name = aimee_pg_column_text(rs, 0);
               if (!name)
                  continue;
               char enc_proj[GRAPH_ENDPOINT_MAX], enc_name[GRAPH_ENDPOINT_MAX];
               db2_entity_node_encode_component(project, enc_proj, sizeof(enc_proj));
               db2_entity_node_encode_component(name, enc_name, sizeof(enc_name));
               char route_key[GRAPH_ENDPOINT_MAX];
               snprintf(route_key, sizeof(route_key), "route:%s:%s", enc_proj, enc_name);
               if (strlen(route_key) >= GRAPH_ENDPOINT_MAX)
                  continue;
               if (project_edge(gen_id, project, file_key, "routes", route_key, REL_OTHER,
                                NODE_FILE, NODE_OTHER) == 0)
                  edge_count++;
            }
            aimee_pg_finalize(rs);
         }
      }

      /* calls: symbol → symbol (from code_calls joined through file) */
      {
         static const char *calls_sql = "SELECT caller, callee FROM code_calls WHERE file_id = ?1"
                                        "   AND caller != '' AND callee != ''";
         aimee_pg_stmt_t *cs = aimee_pg_prepare(conn, calls_sql, err, sizeof(err));
         if (cs)
         {
            aimee_pg_bind_int64(cs, "?1", files[fi].id);
            while (aimee_pg_step(cs, err, sizeof(err)) == AIMEE_PG_ROW)
            {
               const char *caller = aimee_pg_column_text(cs, 0);
               const char *callee = aimee_pg_column_text(cs, 1);
               if (!caller || !callee)
                  continue;
               char caller_key[GRAPH_ENDPOINT_MAX], callee_key[GRAPH_ENDPOINT_MAX];
               if (db2_entity_node_key_symbol(project, caller, caller_key, sizeof(caller_key)) != 0)
                  continue;
               if (db2_entity_node_key_symbol(project, callee, callee_key, sizeof(callee_key)) != 0)
                  continue;
               if (project_edge(gen_id, project, caller_key, "calls", callee_key, REL_CALLS,
                                NODE_FUNCTION, NODE_FUNCTION) == 0)
                  edge_count++;
            }
            aimee_pg_finalize(cs);
         }
      }
   }

   free(files);
   db2_code_projection_generation_update_counts(gen_id, edge_count, file_count);
   return edge_count;
}
