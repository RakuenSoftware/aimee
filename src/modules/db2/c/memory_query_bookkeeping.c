/* db2/memory_query_bookkeeping.c: per-memory-id delete + list helpers,
 * Postgres via libpq. Forward declarations remain in memory_query.h. */

#include "../headers/aimee.h" /* memory_t and per-memory row structs */
#include "memory_query.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MQB_ERRBUF 256

void db2_memory_provenance_delete(int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "DELETE FROM memory_provenance WHERE memory_id = ?1", err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_unit_list_ids(int64_t memory_id, int64_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT id FROM memory_units WHERE memory_id = ?1", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

static void mq_delete_by_memory_id(const char *table, int64_t memory_id)
{
   if (memory_id <= 0 || !table)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   char sql[128];
   snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE memory_id = ?1", table);
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_entities_delete_for_memory(int64_t memory_id)
{
   mq_delete_by_memory_id("memory_entities", memory_id);
}

void db2_memory_temporal_refs_delete_for_memory(int64_t memory_id)
{
   mq_delete_by_memory_id("memory_temporal_refs", memory_id);
}

int db2_memory_get_source_session(int64_t memory_id, char *out, int out_len)
{
   if (memory_id <= 0 || !out || out_len <= 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT source_session FROM memories WHERE id = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      if (s && s[0])
      {
         snprintf(out, out_len, "%s", s);
         rc = 0;
      }
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_list_prior_in_session(const char *session_id, int64_t before_id, int limit,
                                     db2_memory_prior_row_t *out, int max)
{
   if (!session_id || !*session_id || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   if (limit <= 0)
      limit = max;

   static const char *sql = "SELECT content, key FROM memories"
                            " WHERE source_session = ?1 AND id < ?2"
                            " ORDER BY id DESC LIMIT ?3";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);
   aimee_pg_bind_int64(st, "?2", before_id);
   aimee_pg_bind_int(st, "?3", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].content, sizeof(out[n].content), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].key, sizeof(out[n].key), aimee_pg_column_text(st, 1));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_lookup_primary_entity(int64_t memory_id, char *out, int out_len)
{
   if (!out || out_len <= 0)
      return;
   out[0] = '\0';
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "SELECT entity FROM memory_entities WHERE memory_id = ?1"
                            " ORDER BY CASE role WHEN 'actor' THEN 0"
                            "                    WHEN 'subject' THEN 1"
                            "                    WHEN 'person' THEN 2"
                            "                    ELSE 3 END,"
                            "          weight DESC, id ASC"
                            " LIMIT 1";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *e = aimee_pg_column_text(st, 0);
      snprintf(out, out_len, "%s", e ? e : "");
   }
   aimee_pg_finalize(st);
}

void db2_memory_relations_delete_for_memory(int64_t memory_id)
{
   mq_delete_by_memory_id("memory_relations", memory_id);
}

void db2_memory_episodes_delete_for_memory(int64_t memory_id)
{
   mq_delete_by_memory_id("memory_episodes", memory_id);
}

int db2_memory_summaries_list(int64_t memory_id, int limit, db2_memory_summary_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   if (limit <= 0)
      limit = max;

   static const char *sql =
       "SELECT scope, summary FROM memory_summaries WHERE memory_id = ?1 ORDER BY id ASC LIMIT ?2";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_int(st, "?2", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].scope, sizeof(out[n].scope), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].summary, sizeof(out[n].summary), aimee_pg_column_text(st, 1));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_retrieval_shortcut_observe(const char *normalized_query, const int64_t *ids, int count)
{
   if (!normalized_query || !normalized_query[0] || !ids || count <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char target_ids[512] = "";
   int pos = 0;
   int n_ids = count > 8 ? 8 : count;
   for (int i = 0; i < n_ids && pos < (int)sizeof(target_ids) - 32; i++)
      pos += snprintf(target_ids + pos, sizeof(target_ids) - (size_t)pos, "%s%lld",
                      i == 0 ? "" : ",", (long long)ids[i]);

   char existing[512] = "";
   int64_t hit_count = 0;
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *sel = aimee_pg_prepare(
       conn, "SELECT target_ids, hit_count FROM retrieval_shortcuts WHERE normalized_query = ?1",
       err, sizeof(err));
   if (sel)
   {
      aimee_pg_bind_text(sel, "?1", normalized_query);
      if (aimee_pg_step(sel, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *v = aimee_pg_column_text(sel, 0);
         if (v)
            snprintf(existing, sizeof(existing), "%s", v);
         hit_count = aimee_pg_column_int64(sel, 1);
      }
      aimee_pg_finalize(sel);
   }

   int64_t next_count = (existing[0] && strcmp(existing, target_ids) == 0) ? hit_count + 1 : 1;
   int promoted = next_count >= 3 ? 1 : 0;
   static const char *sql =
       "INSERT INTO retrieval_shortcuts"
       " (normalized_query, target_ids, hit_count, promoted, last_used_at, updated_at)"
       " VALUES (?1, ?2, ?3, ?4, pg_now_text(), pg_now_text())"
       " ON CONFLICT (normalized_query) DO UPDATE SET"
       " target_ids = excluded.target_ids,"
       " hit_count = excluded.hit_count,"
       " promoted = excluded.promoted,"
       " last_used_at = excluded.last_used_at,"
       " updated_at = excluded.updated_at";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", normalized_query);
   aimee_pg_bind_text(st, "?2", target_ids);
   aimee_pg_bind_int64(st, "?3", next_count);
   aimee_pg_bind_int(st, "?4", promoted);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_retrieval_shortcut_lookup(const char *normalized_query, int64_t *ids, int max,
                                  int *promoted_out, int64_t *hit_count_out)
{
   if (promoted_out)
      *promoted_out = 0;
   if (hit_count_out)
      *hit_count_out = 0;
   if (!normalized_query || !normalized_query[0] || !ids || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT target_ids, promoted, hit_count FROM retrieval_shortcuts WHERE "
                            "normalized_query = ?1";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", normalized_query);
   int n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *targets = aimee_pg_column_text(st, 0);
      if (promoted_out)
         *promoted_out = aimee_pg_column_int(st, 1);
      if (hit_count_out)
         *hit_count_out = aimee_pg_column_int64(st, 2);
      const char *p = targets ? targets : "";
      while (*p && n < max)
      {
         char *end = NULL;
         long long id = strtoll(p, &end, 10);
         if (end == p)
            break;
         if (id > 0)
            ids[n++] = (int64_t)id;
         p = (*end == ',') ? end + 1 : end;
      }
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_event_frames_list(int64_t memory_id, db2_memory_event_frame_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT actor, action, object, location, event_time FROM memory_event_frames"
       " WHERE memory_id = ?1 ORDER BY id ASC";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].actor, sizeof(out[n].actor), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].action, sizeof(out[n].action), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].object, sizeof(out[n].object), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].location, sizeof(out[n].location), aimee_pg_column_text(st, 3));
      db2_copy_text(out[n].event_time, sizeof(out[n].event_time), aimee_pg_column_text(st, 4));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_unit_edges_delete_for_memory(int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "DELETE FROM memory_unit_edges"
       " WHERE src_unit_id IN (SELECT id FROM memory_units WHERE memory_id = ?1)"
       "    OR dst_unit_id IN (SELECT id FROM memory_units WHERE memory_id = ?2)";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_int64(st, "?2", memory_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_units_delete_for_memory(int64_t memory_id)
{
   mq_delete_by_memory_id("memory_units", memory_id);
}

int db2_memory_get_session_kinds(int64_t memory_id, char *session_out, int session_len,
                                 char *kind_out, int kind_len, char *explicit_kind_out,
                                 int explicit_kind_len)
{
   if (session_out && session_len > 0)
      session_out[0] = '\0';
   if (kind_out && kind_len > 0)
      kind_out[0] = '\0';
   if (explicit_kind_out && explicit_kind_len > 0)
      explicit_kind_out[0] = '\0';
   if (memory_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT source_session, kind, cognified_memory_kind FROM memories WHERE id = ?1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (session_out && session_len > 0)
         db2_copy_text(session_out, session_len, aimee_pg_column_text(st, 0));
      if (kind_out && kind_len > 0)
         db2_copy_text(kind_out, kind_len, aimee_pg_column_text(st, 1));
      if (explicit_kind_out && explicit_kind_len > 0)
         db2_copy_text(explicit_kind_out, explicit_kind_len, aimee_pg_column_text(st, 2));
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_temporal_refs_list(int64_t memory_id, db2_memory_temporal_ref_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT ref_key, granularity, weight FROM memory_temporal_refs"
                            " WHERE memory_id = ?1 ORDER BY weight DESC, id ASC";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].ref_key, sizeof(out[n].ref_key), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].granularity, sizeof(out[n].granularity), aimee_pg_column_text(st, 1));
      out[n].weight = aimee_pg_column_double(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_entities_list_weighted(int64_t memory_id, db2_memory_entity_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT entity, role, weight FROM memory_entities"
                            " WHERE memory_id = ?1 ORDER BY weight DESC, id ASC";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].entity, sizeof(out[n].entity), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].role, sizeof(out[n].role), aimee_pg_column_text(st, 1));
      out[n].weight = aimee_pg_column_double(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_chunks_list(int64_t memory_id, db2_memory_chunk_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT chunk_text, chunk_index FROM memory_chunks"
                            " WHERE memory_id = ?1 ORDER BY chunk_index ASC";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].chunk_text, sizeof(out[n].chunk_text), aimee_pg_column_text(st, 0));
      out[n].chunk_index = aimee_pg_column_int(st, 1);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_episode_unit_ids_for_session(int64_t memory_id, const char *source_session,
                                            int64_t *out, int max)
{
   if (memory_id <= 0 || !source_session || !*source_session || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT u.id FROM memory_units u"
       "  JOIN memories m ON m.id = u.memory_id"
       " WHERE u.unit_type = 'episode' AND u.memory_id != ?1 AND m.source_session = ?2";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", source_session);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_episode_unit_ids_supersedes(int64_t source_memory_id, int64_t *out, int max)
{
   if (source_memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT u.id FROM memory_links ml"
       "  JOIN memory_units u ON u.memory_id = ml.target_id"
       " WHERE ml.source_id = ?1 AND ml.relation = 'supersedes' AND u.unit_type = 'episode'";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", source_memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_alias_insert(int64_t memory_id, const char *alias, double weight)
{
   if (memory_id <= 0 || !alias || !*alias)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "INSERT INTO memory_aliases (memory_id, alias, weight)"
                            " VALUES (?1, ?2, ?3)"
                            " ON CONFLICT (memory_id, alias) DO NOTHING";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", alias);
   aimee_pg_bind_double(st, "?3", weight);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_summary_upsert(int64_t memory_id, const char *scope, const char *summary)
{
   if (memory_id <= 0 || !summary || !*summary)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO memory_summaries (memory_id, scope, summary)"
       " VALUES (?1, ?2, ?3)"
       " ON CONFLICT (memory_id, scope) DO UPDATE SET summary = EXCLUDED.summary";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", (scope && scope[0]) ? scope : "headline");
   aimee_pg_bind_text(st, "?3", summary);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_summaries_delete_for_memory(int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "DELETE FROM memory_summaries WHERE memory_id = ?1";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_chunk_upsert(int64_t memory_id, int chunk_index, const char *chunk_text)
{
   if (memory_id <= 0 || !chunk_text || !*chunk_text)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO memory_chunks (memory_id, chunk_index, chunk_text)"
       " VALUES (?1, ?2, ?3)"
       " ON CONFLICT (memory_id, chunk_index) DO UPDATE SET chunk_text = EXCLUDED.chunk_text";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_int(st, "?2", chunk_index);
   aimee_pg_bind_text(st, "?3", chunk_text);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_event_frame_insert(int64_t memory_id, const char *actor, const char *action,
                                   const char *object, const char *location, const char *event_time,
                                   const char *evidence_kind)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO memory_event_frames"
       " (memory_id, actor, action, object, location, event_time, evidence_kind)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", actor ? actor : "");
   aimee_pg_bind_text(st, "?3", action ? action : "");
   aimee_pg_bind_text(st, "?4", object ? object : "");
   aimee_pg_bind_text(st, "?5", location ? location : "");
   aimee_pg_bind_text(st, "?6", event_time ? event_time : "");
   aimee_pg_bind_text(st, "?7", (evidence_kind && evidence_kind[0]) ? evidence_kind : "derived");
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void mqb_delete_by_memory_id(const char *sql, int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[MQB_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_event_frames_delete_for_memory(int64_t memory_id)
{
   mqb_delete_by_memory_id("DELETE FROM memory_event_frames WHERE memory_id = ?1", memory_id);
}

void db2_memory_chunks_delete_for_memory(int64_t memory_id)
{
   mqb_delete_by_memory_id("DELETE FROM memory_chunks WHERE memory_id = ?1", memory_id);
}

void db2_memory_aliases_delete_for_memory(int64_t memory_id)
{
   mqb_delete_by_memory_id("DELETE FROM memory_aliases WHERE memory_id = ?1", memory_id);
}
