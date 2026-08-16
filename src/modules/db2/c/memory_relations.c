/* db2/memory_relations.c: SQL primitives for memory_links / memory_provenance /
 * memory_relations / memory_lineage — Postgres via libpq. */

#include "../headers/aimee.h" /* memory_link_t + provenance_entry_t + memory_t */
#include "memory_query.h"     /* db2_memory_collect_relation_token_matches forward decl */
#include "memory_relations.h"
#include "memory_scope_query.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define MR_ERRBUF 256

/* Keep the entity-profile query readable: these named fragments avoid forcing
 * the formatter to break macro-expanded SQL into tiny adjacent literals. */
#define MR_PROFILE_MENTION_SCOPE  DB2_MEMORY_SCOPE_FILTER_SQL("men.memory_id")
#define MR_PROFILE_RELATION_SCOPE DB2_MEMORY_SCOPE_FILTER_SQL("mrc.memory_id")
#define MR_PROFILE_EPISODE_SCOPE  DB2_MEMORY_SCOPE_FILTER_SQL("mre.memory_id")
#define MR_PROFILE_EPISODE_RANK   DB2_MEMORY_SCOPE_RANK_SQL("mre.memory_id")
#define MR_PROFILE_SUMMARY_SCOPE  DB2_MEMORY_SCOPE_FILTER_SQL("mrs.memory_id")
#define MR_PROFILE_SUMMARY_RANK   DB2_MEMORY_SCOPE_RANK_SQL("mrs.memory_id")

int db2_memory_link_create(int64_t source_id, int64_t target_id, const char *relation)
{
   if (source_id <= 0 || target_id <= 0 || !relation)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO memory_links (source_id, target_id, relation)"
                            " VALUES (?1, ?2, ?3) ON CONFLICT DO NOTHING";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", source_id);
   aimee_pg_bind_int64(st, "?2", target_id);
   aimee_pg_bind_text(st, "?3", relation);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_link_query(int64_t memory_id, memory_link_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, source_id, target_id, relation, created_at"
                            "  FROM memory_links"
                            " WHERE source_id = ?1 OR target_id = ?2"
                            " ORDER BY created_at DESC LIMIT ?3";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_int64(st, "?2", memory_id);
   aimee_pg_bind_int(st, "?3", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      out[n].source_id = aimee_pg_column_int64(st, 1);
      out[n].target_id = aimee_pg_column_int64(st, 2);
      const char *rel = aimee_pg_column_text(st, 3);
      snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel ? rel : "");
      const char *ts = aimee_pg_column_text(st, 4);
      snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", ts ? ts : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_link_delete(int64_t link_id)
{
   if (link_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "DELETE FROM memory_links WHERE id = ?1";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", link_id);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_provenance_list(int64_t memory_id, provenance_entry_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, memory_id, session_id, action, details, created_at"
                            "  FROM memory_provenance WHERE memory_id = ?1"
                            " ORDER BY created_at ASC";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      provenance_entry_t *e = &out[n];
      e->id = aimee_pg_column_int64(st, 0);
      e->memory_id = aimee_pg_column_int64(st, 1);
      const char *s;
      s = aimee_pg_column_text(st, 2);
      snprintf(e->session_id, sizeof(e->session_id), "%s", s ? s : "");
      s = aimee_pg_column_text(st, 3);
      snprintf(e->action, sizeof(e->action), "%s", s ? s : "");
      s = aimee_pg_column_text(st, 4);
      snprintf(e->details, sizeof(e->details), "%s", s ? s : "");
      s = aimee_pg_column_text(st, 5);
      snprintf(e->created_at, sizeof(e->created_at), "%s", s ? s : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_provenance_insert(int64_t memory_id, const char *session_id, const char *action,
                                  const char *details)
{
   if (memory_id <= 0 || !action)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "INSERT INTO memory_provenance (memory_id, session_id, action,"
                            " details, created_at) VALUES (?1, ?2, ?3, ?4, ?5)";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;

   char ts[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", session_id ? session_id : "");
   aimee_pg_bind_text(st, "?3", action);
   aimee_pg_bind_text(st, "?4", details ? details : "");
   aimee_pg_bind_text(st, "?5", ts);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_relation_upsert_full(int64_t memory_id, int64_t episode_id, const char *src_entity,
                                     const char *relation, const char *dst_entity,
                                     const char *fact_text, const char *valid_at,
                                     const char *invalid_at, double weight)
{
   if (memory_id <= 0 || !relation || !*relation || !fact_text || !*fact_text)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   /* memory_relations has no UNIQUE constraint to drive ON CONFLICT; preserve
    * replace semantics by deleting any existing
    * (memory_id, src_entity, relation, dst_entity) row first, then inserting
    * fresh. */
   {
      static const char *del_sql =
          "DELETE FROM memory_relations WHERE memory_id = ?1 AND src_entity = ?2"
          " AND relation = ?3 AND dst_entity = ?4";
      char err[MR_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, del_sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", memory_id);
         aimee_pg_bind_text(st, "?2", src_entity ? src_entity : "");
         aimee_pg_bind_text(st, "?3", relation);
         aimee_pg_bind_text(st, "?4", dst_entity ? dst_entity : "");
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
   }

   static const char *sql = "INSERT INTO memory_relations"
                            "  (memory_id, episode_id, src_entity, relation, dst_entity, fact_text,"
                            "   valid_at, invalid_at, weight)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   if (episode_id > 0)
      aimee_pg_bind_int64(st, "?2", episode_id);
   else
      aimee_pg_bind_null(st, "?2");
   aimee_pg_bind_text(st, "?3", src_entity ? src_entity : "");
   aimee_pg_bind_text(st, "?4", relation);
   aimee_pg_bind_text(st, "?5", dst_entity ? dst_entity : "");
   aimee_pg_bind_text(st, "?6", fact_text);
   aimee_pg_bind_text(st, "?7", valid_at ? valid_at : "");
   aimee_pg_bind_text(st, "?8", invalid_at ? invalid_at : "");
   aimee_pg_bind_double(st, "?9", weight);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_relation_insert(int64_t memory_id, const char *src_entity, const char *relation,
                                const char *dst_entity, const char *fact_text)
{
   if (memory_id <= 0 || !relation)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "INSERT INTO memory_relations"
                            " (memory_id, src_entity, relation, dst_entity, fact_text)"
                            " VALUES (?1, ?2, ?3, ?4, ?5)";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", src_entity ? src_entity : "");
   aimee_pg_bind_text(st, "?3", relation);
   aimee_pg_bind_text(st, "?4", dst_entity ? dst_entity : "");
   aimee_pg_bind_text(st, "?5", fact_text ? fact_text : "");
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_collect_relation_token_matches(const char *token, int limit, memory_t *out, int max)
{
   if (!token || !token[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* GROUP BY on the full memories projection requires every non-aggregated
    * column to be listed explicitly. */
   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       " m.last_used_at, m.created_at, m.updated_at, m.source_session, m.salience, "
       "m.provenance_category"
       " FROM memory_relations r"
       " JOIN memories m ON m.id = r.memory_id"
       " WHERE r.memory_id > 0"
       "   AND (LOWER(r.src_entity) = LOWER(?1)"
       "        OR LOWER(r.dst_entity) = LOWER(?2)"
       "        OR LOWER(r.relation) = LOWER(?3)"
       "        OR LOWER(r.fact_text) LIKE '%' || LOWER(?4) || '%')" DB2_MEMORY_SCOPE_FILTER_SQL(
           "m.id") " GROUP BY m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
                   "          m.last_used_at, m.created_at, m.updated_at, m.source_session, "
                   "m.salience, "
                   "m.provenance_category"
                   " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL(
                       "m.id") " DESC, MAX(r.weight) DESC, m.confidence DESC, m.use_count DESC"
                               " LIMIT ?5";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", token);
   aimee_pg_bind_text(st, "?2", token);
   aimee_pg_bind_text(st, "?3", token);
   aimee_pg_bind_text(st, "?4", token);
   aimee_pg_bind_int(st, "?5", limit);
   db2_memory_scope_bind_current(st);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static void db2_memory_relation_fill_pg(aimee_pg_stmt_t *st, memory_relation_t *out)
{
   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   out->memory_id = aimee_pg_column_int64(st, 1);
   out->episode_id = aimee_pg_column_int64(st, 2);
   const char *s;
   s = aimee_pg_column_text(st, 3);
   snprintf(out->src_entity, sizeof(out->src_entity), "%s", s ? s : "");
   s = aimee_pg_column_text(st, 4);
   snprintf(out->relation, sizeof(out->relation), "%s", s ? s : "");
   s = aimee_pg_column_text(st, 5);
   snprintf(out->dst_entity, sizeof(out->dst_entity), "%s", s ? s : "");
   s = aimee_pg_column_text(st, 6);
   snprintf(out->fact_text, sizeof(out->fact_text), "%s", s ? s : "");
   s = aimee_pg_column_text(st, 7);
   snprintf(out->valid_at, sizeof(out->valid_at), "%s", s ? s : "");
   s = aimee_pg_column_text(st, 8);
   snprintf(out->invalid_at, sizeof(out->invalid_at), "%s", s ? s : "");
   out->weight = aimee_pg_column_double(st, 9);
   s = aimee_pg_column_text(st, 10);
   snprintf(out->created_at, sizeof(out->created_at), "%s", s ? s : "");
}

int db2_memory_relations_search(const char *query, int limit, memory_relation_t *out, int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT r.id, r.memory_id, COALESCE(r.episode_id, 0), r.src_entity, r.relation,"
       " r.dst_entity, r.fact_text, r.valid_at, r.invalid_at, r.weight, r.created_at"
       " FROM memory_relations r"
       " WHERE (LOWER(r.src_entity) LIKE '%' || LOWER(?1) || '%'"
       "    OR LOWER(r.relation)   LIKE '%' || LOWER(?2) || '%'"
       "    OR LOWER(r.dst_entity) LIKE '%' || LOWER(?3) || '%'"
       "    OR LOWER(r.fact_text)  LIKE '%' || LOWER(?4) || '%')" DB2_MEMORY_SCOPE_FILTER_SQL(
           "r.memory_id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL("r.memory_id") " DESC, r.weight "
                                                                                "DESC, CASE WHEN "
                                                                                "r.valid_at <> '' "
                                                                                "THEN 0 ELSE 1 "
                                                                                "END, r.created_at "
                                                                                "DESC LIMIT "
                                                                                "?5";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", query);
   aimee_pg_bind_text(st, "?2", query);
   aimee_pg_bind_text(st, "?3", query);
   aimee_pg_bind_text(st, "?4", query);
   aimee_pg_bind_int(st, "?5", limit);
   db2_memory_scope_bind_current(st);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      db2_memory_relation_fill_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_relations_for_entity(const char *entity, int limit, memory_relation_t *out, int max)
{
   if (!entity || !entity[0] || !out || max <= 0)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT r.id, r.memory_id, COALESCE(r.episode_id, 0), r.src_entity, r.relation,"
       " r.dst_entity, r.fact_text, r.valid_at, r.invalid_at, r.weight, r.created_at"
       " FROM memory_relations r"
       " WHERE (LOWER(r.src_entity) = LOWER(?1) OR LOWER(r.dst_entity) = "
       "LOWER(?2))" DB2_MEMORY_SCOPE_FILTER_SQL(
           "r.memory_id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL("r.memory_id") " DESC, r.weight "
                                                                                "DESC, "
                                                                                "r.created_at DESC "
                                                                                "LIMIT ?3";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   aimee_pg_bind_int(st, "?3", limit);
   db2_memory_scope_bind_current(st);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      db2_memory_relation_fill_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_entity_profile_stats(const char *normalized_entity, int *mention_count,
                                    int *relation_count, char *latest_episode,
                                    int latest_episode_len, char *summary, int summary_len)
{
   if (!normalized_entity || !normalized_entity[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT"
       " (SELECT COUNT(DISTINCT men.memory_id) FROM memory_entities men"
       "   WHERE LOWER(men.entity) = LOWER(?1)" MR_PROFILE_MENTION_SCOPE "),"
       " (SELECT COUNT(*) FROM memory_relations mrc"
       "   WHERE (LOWER(mrc.src_entity) = LOWER(?2)"
       "      OR LOWER(mrc.dst_entity) = LOWER(?3))" MR_PROFILE_RELATION_SCOPE "),"
       " COALESCE((SELECT me.episode_key FROM memory_episodes me"
       "            JOIN memory_relations mre ON mre.episode_id = me.id"
       "            WHERE (LOWER(mre.src_entity) = LOWER(?4)"
       "               OR LOWER(mre.dst_entity) = LOWER(?5))" MR_PROFILE_EPISODE_SCOPE
       "            ORDER BY " MR_PROFILE_EPISODE_RANK " DESC, me.created_at DESC LIMIT 1), ''),"
       " COALESCE((SELECT mrs.fact_text FROM memory_relations mrs"
       "            WHERE (LOWER(mrs.src_entity) = LOWER(?6)"
       "               OR LOWER(mrs.dst_entity) = LOWER(?7))" MR_PROFILE_SUMMARY_SCOPE
       "            ORDER BY " MR_PROFILE_SUMMARY_RANK
       " DESC, mrs.weight DESC, mrs.created_at DESC LIMIT 1), '')";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", normalized_entity);
   aimee_pg_bind_text(st, "?2", normalized_entity);
   aimee_pg_bind_text(st, "?3", normalized_entity);
   aimee_pg_bind_text(st, "?4", normalized_entity);
   aimee_pg_bind_text(st, "?5", normalized_entity);
   aimee_pg_bind_text(st, "?6", normalized_entity);
   aimee_pg_bind_text(st, "?7", normalized_entity);
   db2_memory_scope_bind_current(st);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int mc = aimee_pg_column_int(st, 0);
      int rc = aimee_pg_column_int(st, 1);
      if (mention_count)
         *mention_count = mc;
      if (relation_count)
         *relation_count = rc;
      const char *le = aimee_pg_column_text(st, 2);
      const char *sm = aimee_pg_column_text(st, 3);
      if (latest_episode && latest_episode_len > 0)
         snprintf(latest_episode, (size_t)latest_episode_len, "%s", le ? le : "");
      if (summary && summary_len > 0)
         snprintf(summary, (size_t)summary_len, "%s", sm ? sm : "");
      hit = (mc > 0 || rc > 0) ? 1 : 0;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_relations_search_as_of(const char *query, const char *as_of, int limit,
                                      memory_relation_t *out, int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   if (!as_of || !as_of[0])
      return db2_memory_relations_search(query, limit, out, max);
   if (limit <= 0 || limit > max)
      limit = max;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT r.id, r.memory_id, COALESCE(r.episode_id, 0), r.src_entity, r.relation,"
       " r.dst_entity, r.fact_text, r.valid_at, r.invalid_at, r.weight, r.created_at"
       " FROM memory_relations r"
       " WHERE (LOWER(r.src_entity) LIKE '%' || LOWER(?1) || '%'"
       "     OR LOWER(r.relation)   LIKE '%' || LOWER(?2) || '%'"
       "     OR LOWER(r.dst_entity) LIKE '%' || LOWER(?3) || '%'"
       "     OR LOWER(r.fact_text)  LIKE '%' || LOWER(?4) || '%')"
       "   AND (r.valid_at  = '' OR r.valid_at  <= ?5)"
       "   AND (r.invalid_at = '' OR r.invalid_at > ?6)" DB2_MEMORY_SCOPE_FILTER_SQL(
           "r.memory_id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL("r.memory_id") " DESC, r.weight "
                                                                                "DESC, "
                                                                                "r.created_at DESC "
                                                                                "LIMIT ?7";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", query);
   aimee_pg_bind_text(st, "?2", query);
   aimee_pg_bind_text(st, "?3", query);
   aimee_pg_bind_text(st, "?4", query);
   aimee_pg_bind_text(st, "?5", as_of);
   aimee_pg_bind_text(st, "?6", as_of);
   aimee_pg_bind_int(st, "?7", limit);
   db2_memory_scope_bind_current(st);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      db2_memory_relation_fill_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int64_t db2_memory_lineage_insert(const char *object_type, int64_t object_id,
                                  const char *source_kind, const char *source_ref,
                                  double confidence)
{
   if (!object_type || !object_type[0] || object_id <= 0 || !source_kind)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "INSERT INTO memory_lineage (object_type, object_id, source_kind, source_ref, confidence)"
       " VALUES (?1, ?2, ?3, ?4, ?5) RETURNING id";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", object_type);
   aimee_pg_bind_int64(st, "?2", object_id);
   aimee_pg_bind_text(st, "?3", source_kind);
   aimee_pg_bind_text(st, "?4", source_ref ? source_ref : "");
   aimee_pg_bind_double(st, "?5", confidence);
   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return new_id;
}

int db2_memory_relations_supporting(const char *entity_token, int limit, memory_relation_t *out,
                                    int max)
{
   if (!entity_token || !entity_token[0] || !out || max <= 0)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char like_term[260];
   snprintf(like_term, sizeof(like_term), "%%%s%%", entity_token);
   static const char *sql =
       "SELECT r.id, r.memory_id, COALESCE(r.episode_id, 0), r.src_entity, r.relation,"
       " r.dst_entity, r.fact_text, r.valid_at, r.invalid_at, r.weight, r.created_at"
       " FROM memory_relations r"
       " WHERE (lower(r.src_entity) LIKE lower(?1) OR lower(r.dst_entity) LIKE lower(?2))"
       " AND r.fact_text != ''" DB2_MEMORY_SCOPE_FILTER_SQL(
           "r.memory_id") " ORDER BY " DB2_MEMORY_SCOPE_RANK_SQL("r.memory_id") " DESC, r.weight "
                                                                                "DESC LIMIT ?3";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", like_term);
   aimee_pg_bind_text(st, "?2", like_term);
   aimee_pg_bind_int(st, "?3", limit);
   db2_memory_scope_bind_current(st);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      db2_memory_relation_fill_pg(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_relation_dates_for_memory(int64_t memory_id, db2_memory_relation_date_row_t *rows,
                                         int max)
{
   if (!rows || max <= 0 || memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT mr.valid_at FROM memory_relations mr"
                            " WHERE mr.memory_id = ?1 AND mr.valid_at != ''"
                            " AND mr.relation IN ('OCCURRED_AT', 'occurred_at', 'valid_from')"
                            " ORDER BY mr.valid_at ASC";
   char err[MR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int n = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && n < max)
   {
      const char *d = aimee_pg_column_text(st, 0);
      if (!d || !d[0])
         continue;
      snprintf(rows[n].date, sizeof(rows[n].date), "%s", d);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
