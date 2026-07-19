/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* db2/memory_query.c: SELECT primitives over memories + memory_units —
 * Postgres via libpq. Shared 12-col row mapper lives in
 * memory_row_mapper_pg.c. */

#include "../headers/aimee.h" /* memory_t */
#include "memory_query.h"
#include "vector_index_ops.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MQ_ERRBUF 256

int db2_memory_fact_history(const char *normalized_key, memory_t *out, int max)
{
   if (!normalized_key || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char like_pattern[560];
   snprintf(like_pattern, sizeof(like_pattern), "%s#v%%", normalized_key);

   static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                            "       last_used_at, created_at, updated_at, source_session"
                            "  FROM memories"
                            " WHERE key = ?1 OR key LIKE ?2"
                            " ORDER BY created_at DESC";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", normalized_key);
   aimee_pg_bind_text(st, "?2", like_pattern);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out[n].tier, sizeof(out[n].tier), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].kind, sizeof(out[n].kind), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].key, sizeof(out[n].key), aimee_pg_column_text(st, 3));
      db2_copy_text(out[n].content, sizeof(out[n].content), aimee_pg_column_text(st, 4));
      out[n].confidence = aimee_pg_column_double(st, 5);
      out[n].use_count = aimee_pg_column_int(st, 6);
      db2_copy_text(out[n].last_used_at, sizeof(out[n].last_used_at), aimee_pg_column_text(st, 7));
      db2_copy_text(out[n].created_at, sizeof(out[n].created_at), aimee_pg_column_text(st, 8));
      db2_copy_text(out[n].updated_at, sizeof(out[n].updated_at), aimee_pg_column_text(st, 9));
      db2_copy_text(out[n].source_session, sizeof(out[n].source_session),
                    aimee_pg_column_text(st, 10));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_retryable_index_failures(int max_attempts, int limit, int64_t *out, int max)
{
   return db2_vector_index_ops_list_retryable_memory_ids(max_attempts, limit, out, max);
}

/* Negation lexical recall via the Postgres-only memory_negation_fts_tsv column
 * (a GENERATED tsvector over negation_tokens, GIN-indexed). Matches memories whose
 * negation tokens overlap the query's synthetic "not_<token>" set, GIN-index-backed
 * rather than the per-candidate semantic fallback. Postgres-only: the sqlite shim
 * cannot represent a GENERATED tsvector, so it returns 0 and the caller keeps its
 * synthetic-semantic path. `neg_tokens` is the extract_negation_tokens() output
 * (space-separated "not_<token>"); plainto_tsquery tokenises it the same way the
 * stored column was built, so the lexemes line up. */
int db2_memory_negation_fts_search(const char *neg_tokens, int limit, memory_t *out, int max)
{
   if (!neg_tokens || !neg_tokens[0] || !out || limit <= 0 || max <= 0)
      return 0;
   if (aimee_pg_is_shim())
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id, tier, kind, key, content, confidence, use_count,"
       " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
       " FROM memories"
       " WHERE memory_negation_fts_tsv @@ plainto_tsquery('simple', ?1)"
       " ORDER BY ts_rank(memory_negation_fts_tsv, plainto_tsquery('simple', ?1)) DESC,"
       "          use_count DESC, confidence DESC"
       " LIMIT ?2";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", neg_tokens);
   aimee_pg_bind_int(st, "?2", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_find_facts_like(const char *query, int limit, memory_t *out, int max)
{
   if (!query || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id, tier, kind, key, content, confidence, use_count,"
       " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
       " FROM memories"
       " WHERE (LOWER(key) LIKE '%' || LOWER(?1) || '%'"
       "    OR LOWER(content) LIKE '%' || LOWER(?2) || '%'"
       "    OR LOWER(COALESCE(use_cases, '')) LIKE '%' || LOWER(?3) || '%')"
       " ORDER BY CASE"
       "            WHEN LOWER(key) = LOWER(?4) THEN 0"
       "            WHEN LOWER(content) = LOWER(?5) THEN 1"
       "            WHEN LOWER(COALESCE(use_cases, '')) = LOWER(?6) THEN 2"
       "            WHEN LOWER(key) LIKE LOWER(?7) || '%' THEN 3"
       "            ELSE 4"
       "          END,"
       "          CASE tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 WHEN 'L1' THEN 2 ELSE 3 END,"
       "          use_count DESC, confidence DESC"
       " LIMIT ?8";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", query);
   aimee_pg_bind_text(st, "?2", query);
   aimee_pg_bind_text(st, "?3", query);
   aimee_pg_bind_text(st, "?4", query);
   aimee_pg_bind_text(st, "?5", query);
   aimee_pg_bind_text(st, "?6", query);
   aimee_pg_bind_text(st, "?7", query);
   aimee_pg_bind_int(st, "?8", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_collect_alias_matches(const char *alias, int limit, memory_t *out, int max)
{
   if (!alias || !alias[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       " m.last_used_at, m.created_at, m.updated_at, m.source_session, m.salience, "
       "m.provenance_category"
       " FROM memory_aliases a"
       " JOIN memories m ON m.id = a.memory_id"
       " WHERE a.alias = ?1"
       "    OR a.alias LIKE ?2 || '%'"
       "    OR a.alias LIKE '%' || ?3 || '%'"
       " ORDER BY CASE"
       "            WHEN a.alias = ?4 THEN 0"
       "            WHEN a.alias LIKE ?5 || '%' THEN 1"
       "            ELSE 2"
       "          END,"
       "          a.weight DESC,"
       "          CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 WHEN 'L1' THEN 2 ELSE 3 END,"
       "          m.use_count DESC, m.confidence DESC"
       " LIMIT ?6";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", alias);
   aimee_pg_bind_text(st, "?2", alias);
   aimee_pg_bind_text(st, "?3", alias);
   aimee_pg_bind_text(st, "?4", alias);
   aimee_pg_bind_text(st, "?5", alias);
   aimee_pg_bind_int(st, "?6", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_collect_entity_matches(const char *term, int limit, memory_t *out, int max)
{
   if (!term || !term[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       " m.last_used_at, m.created_at, m.updated_at, m.source_session, m.salience, "
       "m.provenance_category"
       " FROM memory_entities e"
       " JOIN memories m ON m.id = e.memory_id"
       " WHERE e.entity = ?1"
       "    OR e.entity LIKE ?2 || '%'"
       "    OR e.entity LIKE '%' || ?3 || '%'"
       " ORDER BY CASE"
       "            WHEN e.entity = ?4 THEN 0"
       "            WHEN e.entity LIKE ?5 || '%' THEN 1"
       "            ELSE 2"
       "          END,"
       "          e.weight DESC, m.confidence DESC, m.use_count DESC"
       " LIMIT ?6";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", term);
   aimee_pg_bind_text(st, "?2", term);
   aimee_pg_bind_text(st, "?3", term);
   aimee_pg_bind_text(st, "?4", term);
   aimee_pg_bind_text(st, "?5", term);
   aimee_pg_bind_int(st, "?6", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_collect_temporal_matches(const char *term, int limit, memory_t *out, int max)
{
   if (!term || !term[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       " m.last_used_at, m.created_at, m.updated_at, m.source_session, m.salience, "
       "m.provenance_category"
       " FROM memory_temporal_refs t"
       " JOIN memories m ON m.id = t.memory_id"
       " WHERE t.ref_key = ?1"
       "    OR t.ref_key LIKE ?2 || '%'"
       " ORDER BY CASE WHEN t.ref_key = ?3 THEN 0 ELSE 1 END,"
       "          t.weight DESC, m.confidence DESC, m.use_count DESC"
       " LIMIT ?4";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", term);
   aimee_pg_bind_text(st, "?2", term);
   aimee_pg_bind_text(st, "?3", term);
   aimee_pg_bind_int(st, "?4", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_collect_summary_matches(const char *term, int limit, memory_t *out, int max)
{
   if (!term || !term[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       " m.last_used_at, m.created_at, m.updated_at, m.source_session, m.salience, "
       "m.provenance_category"
       " FROM memory_summaries s"
       " JOIN memories m ON m.id = s.memory_id"
       " WHERE LOWER(s.summary) LIKE '%' || LOWER(?1) || '%'"
       " ORDER BY CASE WHEN LOWER(s.summary) LIKE LOWER(?2) || '%' THEN 0 ELSE 1 END,"
       "          m.confidence DESC, m.use_count DESC LIMIT ?3";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", term);
   aimee_pg_bind_text(st, "?2", term);
   aimee_pg_bind_int(st, "?3", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_collect_event_frame_matches(const char *term, int limit, memory_t *out, int max)
{
   if (!term || !term[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       " m.last_used_at, m.created_at, m.updated_at, m.source_session, m.salience, "
       "m.provenance_category"
       " FROM memory_event_frames e"
       " JOIN memories m ON m.id = e.memory_id"
       " WHERE e.actor = ?1 OR e.action = ?2 OR e.object LIKE '%' || ?3 || '%'"
       "    OR e.location = ?4 OR e.event_time = ?5"
       " ORDER BY m.confidence DESC, m.use_count DESC LIMIT ?6";

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", term);
   aimee_pg_bind_text(st, "?2", term);
   aimee_pg_bind_text(st, "?3", term);
   aimee_pg_bind_text(st, "?4", term);
   aimee_pg_bind_text(st, "?5", term);
   aimee_pg_bind_int(st, "?6", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* db2_memory_collect_chunk_matches has moved to pgvector. Chunks aren't
 * separately upserted yet, so the retrieval falls back to memory-level
 * semantic search (memory_core_search.inc::memory_collect_chunk_matches
 * -> memory_collect_memory_matches_via_vector). Future work: index chunks
 * under record_type='chunk' for true chunk-level granularity. */

/* db2_memory_collect_unit_matches has moved to pgvector. The retrieval
 * now lives in memory_core_search.inc as memory_collect_unit_matches_via_vector
 * — embed the query, search pgvector for record_type='unit', group unit hits
 * by parent memory, require >=2 corroborating units per memory. */

int db2_memory_unit_edge_exists(int64_t unit_id_a, int64_t unit_id_b)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT 1 FROM memory_unit_edges WHERE ((src_unit_id = ?1 AND dst_unit_id = ?2)"
       " OR (src_unit_id = ?3 AND dst_unit_id = ?4)) LIMIT 1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", unit_id_a);
   aimee_pg_bind_int64(st, "?2", unit_id_b);
   aimee_pg_bind_int64(st, "?3", unit_id_b);
   aimee_pg_bind_int64(st, "?4", unit_id_a);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}

static int db2_memory_session_neighbors(const char *sql, const char *session_id, int64_t pivot_id,
                                        int limit, memory_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || limit <= 0 || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);
   aimee_pg_bind_int64(st, "?2", pivot_id);
   aimee_pg_bind_int(st, "?3", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_session_neighbors_before(const char *session_id, int64_t before_id, int limit,
                                        memory_t *out, int max)
{
   static const char *sql =
       "SELECT id, tier, kind, key, content, confidence, use_count,"
       " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
       " FROM memories"
       " WHERE source_session = ?1 AND id < ?2 AND id > 0"
       " ORDER BY id DESC LIMIT ?3";
   return db2_memory_session_neighbors(sql, session_id, before_id, limit, out, max);
}

int db2_memory_session_neighbors_after(const char *session_id, int64_t after_id, int limit,
                                       memory_t *out, int max)
{
   static const char *sql =
       "SELECT id, tier, kind, key, content, confidence, use_count,"
       " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
       " FROM memories"
       " WHERE source_session = ?1 AND id > ?2"
       " ORDER BY id ASC LIMIT ?3";
   return db2_memory_session_neighbors(sql, session_id, after_id, limit, out, max);
}

int db2_memory_episodes_search(const char *query, int limit, memory_episode_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   static const char *sql =
       "SELECT id, memory_id, episode_key, episode_text, source_session, reference_time, created_at"
       " FROM memory_episodes"
       " WHERE (?1 = '' OR LOWER(episode_key) LIKE '%' || LOWER(?2) || '%'"
       "    OR LOWER(episode_text) LIKE '%' || LOWER(?3) || '%'"
       "    OR LOWER(source_session) LIKE '%' || LOWER(?4) || '%')"
       " ORDER BY CASE WHEN LOWER(episode_key) = LOWER(?5) THEN 0 ELSE 1 END,"
       "          CASE WHEN reference_time <> '' THEN 0 ELSE 1 END,"
       "          created_at DESC LIMIT ?6";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   const char *q = query ? query : "";
   aimee_pg_bind_text(st, "?1", q);
   aimee_pg_bind_text(st, "?2", q);
   aimee_pg_bind_text(st, "?3", q);
   aimee_pg_bind_text(st, "?4", q);
   aimee_pg_bind_text(st, "?5", q);
   aimee_pg_bind_int(st, "?6", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      out[n].memory_id = aimee_pg_column_int64(st, 1);
      db2_copy_text(out[n].episode_key, sizeof(out[n].episode_key), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].episode_text, sizeof(out[n].episode_text), aimee_pg_column_text(st, 3));
      db2_copy_text(out[n].source_session, sizeof(out[n].source_session),
                    aimee_pg_column_text(st, 4));
      db2_copy_text(out[n].reference_time, sizeof(out[n].reference_time),
                    aimee_pg_column_text(st, 5));
      db2_copy_text(out[n].created_at, sizeof(out[n].created_at), aimee_pg_column_text(st, 6));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_filter_archived_ids(const int64_t *ids, int n, int64_t *out, int max)
{
   if (!ids || n <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* Build "?1,?2,?3,..." with one positional placeholder per input id so
    * we can use aimee_pg_bind_int64 instead of stringifying the IN clause. */
   char placeholders[4096];
   int pos = 0;
   for (int i = 0; i < n && pos + 16 < (int)sizeof(placeholders); i++)
      pos += snprintf(placeholders + pos, sizeof(placeholders) - (size_t)pos,
                      i == 0 ? "?%d" : ",?%d", i + 1);
   char sql[4400];
   snprintf(sql, sizeof(sql),
            "SELECT id FROM memories WHERE id IN (%s) AND lifecycle_state = 'archived'",
            placeholders);

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   for (int i = 0; i < n; i++)
   {
      char name[16];
      snprintf(name, sizeof(name), "?%d", i + 1);
      aimee_pg_bind_int64(st, name, ids[i]);
   }

   int written = 0;
   while (written < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[written++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return written;
}

int db2_memory_unit_active_meta(int64_t unit_id, double *weight_out, char *unit_type_out,
                                int unit_type_len, char *unit_kind_out, int unit_kind_len)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT u.weight, u.unit_type, u.memory_kind"
                            " FROM memory_units u JOIN memories m ON m.id = u.memory_id"
                            " WHERE u.id = ?1 AND m.tier IN ('L1', 'L2', 'L3')";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", unit_id);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (weight_out)
         *weight_out = aimee_pg_column_double(st, 0);
      const char *utype = aimee_pg_column_text(st, 1);
      const char *ukind = aimee_pg_column_text(st, 2);
      if (unit_type_out && unit_type_len > 0)
         snprintf(unit_type_out, (size_t)unit_type_len, "%s", utype ? utype : "");
      if (unit_kind_out && unit_kind_len > 0)
         snprintf(unit_kind_out, (size_t)unit_kind_len, "%s", ukind ? ukind : "");
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_pick_first_temporal_ref(int64_t memory_id, char *out, int out_len)
{
   if (!out || out_len <= 0)
      return 0;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT ref_key FROM memory_temporal_refs WHERE memory_id = ?1"
                            " ORDER BY CASE granularity"
                            "          WHEN 'date_phrase' THEN 0"
                            "          WHEN 'absolute_day' THEN 1"
                            "          WHEN 'month' THEN 2"
                            "          WHEN 'weekday' THEN 3"
                            "          WHEN 'year' THEN 4"
                            "          ELSE 5 END,"
                            "          weight DESC, id ASC LIMIT 1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ref = aimee_pg_column_text(st, 0);
      if (ref && ref[0])
      {
         snprintf(out, (size_t)out_len, "%s", ref);
         hit = 1;
      }
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_search_by_pattern(const char *pattern, db2_memory_search_match_t *out, int max)
{
   if (!pattern || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, key, content FROM memories"
                            " WHERE (tier = 'L1' OR tier = 'L2')"
                            "   AND (key LIKE ?1 OR content LIKE ?2)"
                            " LIMIT ?3";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", pattern);
   aimee_pg_bind_text(st, "?2", pattern);
   aimee_pg_bind_int(st, "?3", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out[n].key, sizeof(out[n].key), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].content, sizeof(out[n].content), aimee_pg_column_text(st, 2));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* 11-col aggregate row mapper: same shape as db2_fill_memory_12col_pg but
 * without salience (the aggregate SELECTs do not fetch m.salience). */
static void agg_fill_row(aimee_pg_stmt_t *st, memory_t *m)
{
   memset(m, 0, sizeof(*m));
   m->id = aimee_pg_column_int64(st, 0);
   db2_copy_text(m->tier, sizeof(m->tier), aimee_pg_column_text(st, 1));
   db2_copy_text(m->kind, sizeof(m->kind), aimee_pg_column_text(st, 2));
   db2_copy_text(m->key, sizeof(m->key), aimee_pg_column_text(st, 3));
   db2_copy_text(m->content, sizeof(m->content), aimee_pg_column_text(st, 4));
   m->confidence = aimee_pg_column_double(st, 5);
   m->use_count = aimee_pg_column_int(st, 6);
   db2_copy_text(m->last_used_at, sizeof(m->last_used_at), aimee_pg_column_text(st, 7));
   db2_copy_text(m->created_at, sizeof(m->created_at), aimee_pg_column_text(st, 8));
   db2_copy_text(m->updated_at, sizeof(m->updated_at), aimee_pg_column_text(st, 9));
   db2_copy_text(m->source_session, sizeof(m->source_session), aimee_pg_column_text(st, 10));
}

int db2_memory_aggregate(const char *entity_seed, const char *keyword, memory_t *out, int max,
                         int *truncated_out)
{
   if (truncated_out)
      *truncated_out = 0;
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* Over-fetch by one to detect truncation without a second query. */
   int fetch_limit = max + 1;

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = NULL;
   int has_entity = entity_seed && entity_seed[0];
   int has_keyword = !has_entity && keyword && keyword[0];

   if (has_entity)
   {
      static const char *sql =
          "SELECT DISTINCT m.id, m.tier, m.kind, m.key, m.content, m.confidence,"
          "       m.use_count, COALESCE(m.last_used_at, m.updated_at),"
          "       m.created_at, m.updated_at, m.source_session"
          "  FROM memories m"
          "  LEFT JOIN memory_entities me ON me.memory_id = m.id"
          " WHERE (me.entity = ?1 OR LOWER(m.content) LIKE ?2 OR LOWER(m.key) LIKE ?3)"
          " ORDER BY COALESCE(m.last_used_at, m.updated_at) DESC, m.id DESC"
          " LIMIT ?4";
      st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return 0;
      char like_pat[MEMORY_AGGREGATION_MAX_ENTITY + 4];
      snprintf(like_pat, sizeof(like_pat), "%%%s%%", entity_seed);
      aimee_pg_bind_text(st, "?1", entity_seed);
      aimee_pg_bind_text(st, "?2", like_pat);
      aimee_pg_bind_text(st, "?3", like_pat);
      aimee_pg_bind_int(st, "?4", fetch_limit);
   }
   else if (has_keyword)
   {
      static const char *sql = "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence,"
                               "       m.use_count, COALESCE(m.last_used_at, m.updated_at),"
                               "       m.created_at, m.updated_at, m.source_session"
                               "  FROM memories m"
                               " WHERE LOWER(m.content) LIKE ?1 OR LOWER(m.key) LIKE ?2"
                               " ORDER BY COALESCE(m.last_used_at, m.updated_at) DESC, m.id DESC"
                               " LIMIT ?3";
      st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return 0;
      char like_pat[160];
      snprintf(like_pat, sizeof(like_pat), "%%%s%%", keyword);
      aimee_pg_bind_text(st, "?1", like_pat);
      aimee_pg_bind_text(st, "?2", like_pat);
      aimee_pg_bind_int(st, "?3", fetch_limit);
   }
   else
   {
      static const char *sql = "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence,"
                               "       m.use_count, COALESCE(m.last_used_at, m.updated_at),"
                               "       m.created_at, m.updated_at, m.source_session"
                               "  FROM memories m"
                               " WHERE m.kind != 'scratch'"
                               " ORDER BY COALESCE(m.last_used_at, m.updated_at) DESC, m.id DESC"
                               " LIMIT ?1";
      st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return 0;
      aimee_pg_bind_int(st, "?1", fetch_limit);
   }

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      agg_fill_row(st, &out[count]);
      count++;
   }
   if (count == max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && truncated_out)
      *truncated_out = 1;
   aimee_pg_finalize(st);
   return count;
}

int64_t db2_memory_unit_insert(int64_t memory_id, const char *unit_type, const char *memory_kind,
                               const char *unit_key, const char *unit_text, double weight)
{
   if (memory_id <= 0 || !unit_type || !*unit_type || !unit_text || !*unit_text)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   const char *kind_str = memory_kind ? memory_kind : "";
   const char *key_str = unit_key ? unit_key : "";

   /* memory_units has UNIQUE(memory_id, unit_type, unit_key, unit_text) — note
    * memory_kind is NOT part of the unique key, so ON CONFLICT DO NOTHING
    * preserves the existing row's memory_kind/weight (matching the prior
    * INSERT OR IGNORE semantics). */
   static const char *ins_sql = "INSERT INTO memory_units"
                                "  (memory_id, unit_type, memory_kind, unit_key, unit_text, weight)"
                                " VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT DO NOTHING";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *ins = aimee_pg_prepare(conn, ins_sql, err, sizeof(err));
   if (!ins)
      return 0;
   aimee_pg_bind_int64(ins, "?1", memory_id);
   aimee_pg_bind_text(ins, "?2", unit_type);
   aimee_pg_bind_text(ins, "?3", kind_str);
   aimee_pg_bind_text(ins, "?4", key_str);
   aimee_pg_bind_text(ins, "?5", unit_text);
   aimee_pg_bind_double(ins, "?6", weight);
   (void)aimee_pg_step(ins, err, sizeof(err));
   aimee_pg_finalize(ins);

   /* Resolve the id for either the freshly-inserted row or an existing
    * matching row. The 5-col WHERE matches the original SELECT exactly. */
   static const char *sel_sql = "SELECT id FROM memory_units"
                                " WHERE memory_id = ?1 AND unit_type = ?2 AND memory_kind = ?3"
                                "   AND unit_key = ?4 AND unit_text = ?5";
   aimee_pg_stmt_t *sel = aimee_pg_prepare(conn, sel_sql, err, sizeof(err));
   if (!sel)
      return 0;
   aimee_pg_bind_int64(sel, "?1", memory_id);
   aimee_pg_bind_text(sel, "?2", unit_type);
   aimee_pg_bind_text(sel, "?3", kind_str);
   aimee_pg_bind_text(sel, "?4", key_str);
   aimee_pg_bind_text(sel, "?5", unit_text);
   int64_t unit_id = 0;
   if (aimee_pg_step(sel, err, sizeof(err)) == AIMEE_PG_ROW)
      unit_id = aimee_pg_column_int64(sel, 0);
   aimee_pg_finalize(sel);
   return unit_id;
}

int db2_memory_links_with_targets(int64_t memory_id, db2_memory_link_target_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT ml.relation, tm.key, tm.content"
                            "  FROM memory_links ml"
                            "  JOIN memories tm ON tm.id = ml.target_id"
                            " WHERE ml.source_id = ?1 ORDER BY ml.id ASC";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      db2_copy_text(out[n].relation, sizeof(out[n].relation), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].target_key, sizeof(out[n].target_key), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].target_content, sizeof(out[n].target_content),
                    aimee_pg_column_text(st, 2));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_unit_edge_insert(int64_t src_unit_id, int64_t dst_unit_id, const char *edge_type,
                                 double weight)
{
   if (src_unit_id <= 0 || dst_unit_id <= 0 || src_unit_id == dst_unit_id || !edge_type ||
       !*edge_type)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   /* memory_unit_edges has no UNIQUE constraint, so the prior INSERT OR
    * IGNORE behaved as a plain INSERT. Preserve that — every call appends
    * a row. Callers that want at-most-one-edge gate on
    * db2_memory_unit_edge_exists first (existing call sites already do). */
   static const char *sql =
       "INSERT INTO memory_unit_edges (src_unit_id, dst_unit_id, edge_type, weight)"
       " VALUES (?1, ?2, ?3, ?4)";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", src_unit_id);
   aimee_pg_bind_int64(st, "?2", dst_unit_id);
   aimee_pg_bind_text(st, "?3", edge_type);
   aimee_pg_bind_double(st, "?4", weight);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_get_by_unit_id(int64_t unit_id, memory_t *out)
{
   if (unit_id <= 0 || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence, m.use_count,"
       "       m.last_used_at, m.created_at, m.updated_at, m.source_session"
       "  FROM memory_units u JOIN memories m ON m.id = u.memory_id"
       " WHERE u.id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", unit_id);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(out, 0, sizeof(*out));
      out->id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out->tier, sizeof(out->tier), aimee_pg_column_text(st, 1));
      db2_copy_text(out->kind, sizeof(out->kind), aimee_pg_column_text(st, 2));
      db2_copy_text(out->key, sizeof(out->key), aimee_pg_column_text(st, 3));
      db2_copy_text(out->content, sizeof(out->content), aimee_pg_column_text(st, 4));
      out->confidence = aimee_pg_column_double(st, 5);
      out->use_count = aimee_pg_column_int(st, 6);
      db2_copy_text(out->last_used_at, sizeof(out->last_used_at), aimee_pg_column_text(st, 7));
      db2_copy_text(out->created_at, sizeof(out->created_at), aimee_pg_column_text(st, 8));
      db2_copy_text(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(st, 9));
      db2_copy_text(out->source_session, sizeof(out->source_session), aimee_pg_column_text(st, 10));
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int64_t db2_memory_episode_insert(int64_t memory_id, const char *episode_key,
                                  const char *episode_text, const char *source_session,
                                  const char *reference_time)
{
   if (memory_id <= 0 || !episode_text || !episode_text[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   const char *ek = episode_key ? episode_key : "";
   const char *ss = source_session ? source_session : "";
   const char *rt = reference_time ? reference_time : "";

   /* memory_episodes has no UNIQUE to drive ON CONFLICT; preserve the prior
    * INSERT OR REPLACE-on-the-quad semantics by deleting any existing row
    * matching (memory_id, episode_key, episode_text, source_session) first
    * and then inserting fresh. Returns the new id via RETURNING. */
   {
      static const char *del_sql =
          "DELETE FROM memory_episodes WHERE memory_id = ?1 AND episode_key = ?2"
          "  AND episode_text = ?3 AND source_session = ?4";
      char err[MQ_ERRBUF] = "";
      aimee_pg_stmt_t *del = aimee_pg_prepare(conn, del_sql, err, sizeof(err));
      if (del)
      {
         aimee_pg_bind_int64(del, "?1", memory_id);
         aimee_pg_bind_text(del, "?2", ek);
         aimee_pg_bind_text(del, "?3", episode_text);
         aimee_pg_bind_text(del, "?4", ss);
         (void)aimee_pg_step(del, err, sizeof(err));
         aimee_pg_finalize(del);
      }
   }

   static const char *ins_sql =
       "INSERT INTO memory_episodes"
       "  (memory_id, episode_key, episode_text, source_session, reference_time)"
       " VALUES (?1, ?2, ?3, ?4, ?5) RETURNING id";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *ins = aimee_pg_prepare(conn, ins_sql, err, sizeof(err));
   if (!ins)
      return 0;
   aimee_pg_bind_int64(ins, "?1", memory_id);
   aimee_pg_bind_text(ins, "?2", ek);
   aimee_pg_bind_text(ins, "?3", episode_text);
   aimee_pg_bind_text(ins, "?4", ss);
   aimee_pg_bind_text(ins, "?5", rt);
   int64_t episode_id = 0;
   if (aimee_pg_step(ins, err, sizeof(err)) == AIMEE_PG_ROW)
      episode_id = aimee_pg_column_int64(ins, 0);
   aimee_pg_finalize(ins);
   return episode_id;
}

void db2_memory_lookup_time_bounds(int64_t memory_id, char *valid_at_out, int valid_len,
                                   char *invalid_at_out, int invalid_len)
{
   if (valid_at_out && valid_len > 0)
      valid_at_out[0] = '\0';
   if (invalid_at_out && invalid_len > 0)
      invalid_at_out[0] = '\0';
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "SELECT COALESCE(valid_from, ''), COALESCE(valid_until, ''),"
       "       COALESCE((SELECT ref_key FROM memory_temporal_refs"
       "                  WHERE memory_id = m.id"
       "                    AND granularity IN ('absolute_day', 'date_phrase', 'year')"
       "                  ORDER BY weight DESC, id ASC LIMIT 1), ''),"
       "       COALESCE(created_at, '')"
       "  FROM memories m WHERE id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *vf = aimee_pg_column_text(st, 0);
      const char *vu = aimee_pg_column_text(st, 1);
      const char *tf = aimee_pg_column_text(st, 2);
      const char *created = aimee_pg_column_text(st, 3);
      if (valid_at_out && valid_len > 0)
      {
         const char *chosen = (vf && vf[0]) ? vf : ((tf && tf[0]) ? tf : created);
         snprintf(valid_at_out, valid_len, "%s", chosen ? chosen : "");
      }
      if (invalid_at_out && invalid_len > 0)
         snprintf(invalid_at_out, invalid_len, "%s", (vu && vu[0]) ? vu : "");
   }
   aimee_pg_finalize(st);
}

int db2_memory_units_list(int64_t memory_id, db2_memory_unit_row_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id, unit_type, unit_key, unit_text, weight FROM memory_units WHERE memory_id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out[n].unit_type, sizeof(out[n].unit_type), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].unit_key, sizeof(out[n].unit_key), aimee_pg_column_text(st, 2));
      db2_copy_text(out[n].unit_text, sizeof(out[n].unit_text), aimee_pg_column_text(st, 3));
      out[n].weight = aimee_pg_column_double(st, 4);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_parse_created_date(int64_t memory_id, int *year, int *month, int *day)
{
   if (memory_id <= 0 || !year || !month || !day)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT created_at FROM memories WHERE id = ?1", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int ok = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *created = aimee_pg_column_text(st, 0);
      if (created && sscanf(created, "%d-%d-%d", year, month, day) == 3)
         ok = 1;
   }
   aimee_pg_finalize(st);
   return ok;
}

int db2_memory_find_conflicting_l2(const char *key, const char *content,
                                   double *existing_confidence_out)
{
   if (existing_confidence_out)
      *existing_confidence_out = 0.0;
   if (!key || !*key || !content)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT confidence FROM memories"
                            " WHERE key = ?1 AND tier = 'L2' AND confidence >= 0.8"
                            "   AND content != ?2 LIMIT 1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_bind_text(st, "?2", content);

   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (existing_confidence_out)
         *existing_confidence_out = aimee_pg_column_double(st, 0);
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind)
{
   if (!key || !*key || !kind || !*kind)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id FROM memories WHERE key = ?1 AND kind = ?2 LIMIT 1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_bind_text(st, "?2", kind);

   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_memory_count_by_tier_kind(db2_memory_tier_kind_count_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT tier, kind, COUNT(*) FROM memories"
                            " GROUP BY tier, kind ORDER BY tier, kind";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *tier = aimee_pg_column_text(st, 0);
      const char *kind = aimee_pg_column_text(st, 1);
      snprintf(out[n].tier, sizeof(out[n].tier), "%s", tier ? tier : "");
      snprintf(out[n].kind, sizeof(out[n].kind), "%s", kind ? kind : "");
      out[n].count = aimee_pg_column_int(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_alloc_all_ids(int64_t **out_ids, size_t *out_count)
{
   if (out_ids)
      *out_ids = NULL;
   if (out_count)
      *out_count = 0;
   if (!out_ids || !out_count)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT id FROM memories";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   int64_t *ids = NULL;
   size_t count = 0;
   size_t cap = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (count == cap)
      {
         size_t ncap = cap ? cap * 2 : 256;
         int64_t *grown = (int64_t *)realloc(ids, ncap * sizeof(*grown));
         if (!grown)
         {
            free(ids);
            aimee_pg_finalize(st);
            return -1;
         }
         ids = grown;
         cap = ncap;
      }
      ids[count++] = aimee_pg_column_int64(st, 0);
   }
   aimee_pg_finalize(st);

   *out_ids = ids;
   *out_count = count;
   return 0;
}

static int db2_memory_session_scope_fill(aimee_pg_stmt_t *st, memory_t *out, int max)
{
   int n = 0;
   char err[MQ_ERRBUF] = "";
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      const char *c = aimee_pg_column_text(st, 2);
      const char *kind = aimee_pg_column_text(st, 3);
      snprintf(out[n].key, sizeof(out[n].key), "%s", k ? k : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", c ? c : "");
      snprintf(out[n].kind, sizeof(out[n].kind), "%s", kind ? kind : "");
      n++;
   }
   return n;
}

int db2_memory_list_session_scope_priority(memory_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.key, m.content, m.kind FROM memories m"
       " WHERE m.tier IN ('L1', 'L2', 'L3')"
       " ORDER BY"
       "   CASE m.kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,"
       "   CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,"
       "   m.use_count DESC LIMIT ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = db2_memory_session_scope_fill(st, out, max);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_session_scope_priority_like(const char *pattern, memory_t *out, int max)
{
   if (!pattern || !*pattern || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT m.id, m.key, m.content, m.kind FROM memories m"
       " WHERE m.tier IN ('L1', 'L2', 'L3')"
       "   AND m.id NOT IN (SELECT memory_id FROM memory_workspaces)"
       "   AND (m.key LIKE ?1 OR m.content LIKE ?2)"
       " ORDER BY"
       "   CASE m.kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,"
       "   CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,"
       "   m.use_count DESC LIMIT ?3";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", pattern);
   aimee_pg_bind_text(st, "?2", pattern);
   aimee_pg_bind_int(st, "?3", max);
   int n = db2_memory_session_scope_fill(st, out, max);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_top_l2_facts(memory_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT key, content FROM memories"
                            " WHERE tier = 'L2' AND kind = 'fact'"
                            " ORDER BY use_count DESC, confidence DESC LIMIT ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      const char *k = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(out[n].key, sizeof(out[n].key), "%s", k ? k : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   if (!key || !*key || !tier_a || !*tier_a || !tier_b || !*tier_b)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT 1 FROM memories WHERE key = ?1"
                            "   AND tier IN (?2, ?3) LIMIT 1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_bind_text(st, "?2", tier_a);
   aimee_pg_bind_text(st, "?3", tier_b);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_search_facts_patterns_by_keyword(const char *keyword, memory_t *out, int max)
{
   if (!keyword || !*keyword || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT key, content FROM memories"
                            " WHERE tier IN ('L2', 'L3', 'L5')"
                            "   AND kind IN ('fact', 'pattern')"
                            "   AND (LOWER(content) LIKE '%' || LOWER(?1) || '%'"
                            "        OR LOWER(key) LIKE '%' || LOWER(?2) || '%')"
                            " ORDER BY confidence DESC, use_count DESC LIMIT ?3";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", keyword);
   aimee_pg_bind_text(st, "?2", keyword);
   aimee_pg_bind_int(st, "?3", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      const char *k = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(out[n].key, sizeof(out[n].key), "%s", k ? k : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list(const char *tier, const char *kind, int hide_archived, int limit, memory_t *out,
                    int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char query[1024];
   int pos = 0;
   pos += snprintf(
       query + pos, sizeof(query) - pos,
       "SELECT id, tier, kind, key, content, confidence, use_count,"
       " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
       " FROM memories WHERE 1=1");
   if (hide_archived && pos < (int)sizeof(query))
      pos += snprintf(query + pos, sizeof(query) - pos, " AND lifecycle_state != 'archived'");
   if (pos >= (int)sizeof(query))
      pos = (int)sizeof(query) - 1;

   int next_idx = 1;
   int tier_idx = 0, kind_idx = 0;
   if (tier && tier[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND tier = ?%d", next_idx);
      if (pos >= (int)sizeof(query))
         pos = (int)sizeof(query) - 1;
      tier_idx = next_idx++;
   }
   if (kind && kind[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND kind = ?%d", next_idx);
      if (pos >= (int)sizeof(query))
         pos = (int)sizeof(query) - 1;
      kind_idx = next_idx++;
   }
   pos += snprintf(query + pos, sizeof(query) - pos, " ORDER BY updated_at DESC");
   if (pos >= (int)sizeof(query))
      pos = (int)sizeof(query) - 1;
   if (limit > 0)
      snprintf(query + pos, sizeof(query) - pos, " LIMIT %d", limit);

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, query, err, sizeof(err));
   if (!st)
      return 0;
   if (tier_idx)
   {
      char name[8];
      snprintf(name, sizeof(name), "?%d", tier_idx);
      aimee_pg_bind_text(st, name, tier);
   }
   if (kind_idx)
   {
      char name[8];
      snprintf(name, sizeof(name), "?%d", kind_idx);
      aimee_pg_bind_text(st, name, kind);
   }

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len)
{
   if (label_out && label_len > 0)
      label_out[0] = '\0';
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const struct
   {
      const char *label;
      const char *sql;
   } plans[] = {
       {"L2 facts",
        "SELECT id, tier, kind, key, content, confidence, use_count,"
        " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
        " FROM memories WHERE tier = 'L2' AND kind = 'fact' LIMIT 100"},
       {"facts",
        "SELECT id, tier, kind, key, content, confidence, use_count,"
        " last_used_at, created_at, updated_at, source_session, salience, provenance_category"
        " FROM memories WHERE kind = 'fact'"
        " AND tier IN ('L1', 'L2', 'L3') LIMIT 100"},
       {"durable memories", "SELECT id, tier, kind, key, content, confidence, use_count,"
                            " last_used_at, created_at, updated_at, source_session,"
                            " salience, provenance_category"
                            " FROM memories WHERE tier IN ('L1', 'L2', 'L3')"
                            " AND kind NOT IN ('scratch') LIMIT 100"}};

   for (size_t p = 0; p < sizeof(plans) / sizeof(plans[0]); p++)
   {
      char err[MQ_ERRBUF] = "";
      aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, plans[p].sql, err, sizeof(err));
      if (!stmt)
         continue;
      memset(out, 0, (size_t)max * sizeof(*out));
      int n = 0;
      while (n < max && aimee_pg_step(stmt, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         db2_fill_memory_12col_pg(stmt, &out[n]);
         n++;
      }
      aimee_pg_finalize(stmt);
      if (n > 0)
      {
         if (label_out && label_len > 0)
            snprintf(label_out, label_len, "%s", plans[p].label);
         return n;
      }
   }
   return 0;
}

int db2_memory_lineage_get(const char *object_type, int64_t object_id, memory_lineage_t *out,
                           int max)
{
   if (!object_type || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id, object_type, object_id, source_kind, source_ref, ingested_at, confidence"
       "  FROM memory_lineage"
       " WHERE object_type = ?1 AND object_id = ?2"
       " ORDER BY id ASC";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", object_type);
   aimee_pg_bind_int64(st, "?2", object_id);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memory_lineage_t *r = &out[n];
      memset(r, 0, sizeof(*r));
      r->id = aimee_pg_column_int64(st, 0);
      db2_copy_text(r->object_type, sizeof(r->object_type), aimee_pg_column_text(st, 1));
      r->object_id = aimee_pg_column_int64(st, 2);
      db2_copy_text(r->source_kind, sizeof(r->source_kind), aimee_pg_column_text(st, 3));
      db2_copy_text(r->source_ref, sizeof(r->source_ref), aimee_pg_column_text(st, 4));
      db2_copy_text(r->ingested_at, sizeof(r->ingested_at), aimee_pg_column_text(st, 5));
      r->confidence = aimee_pg_column_double(st, 6);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_delete_row(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM memories WHERE id = ?1", err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int changes = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_get(int64_t memory_id, memory_t *out)
{
   if (memory_id <= 0 || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                            "       last_used_at, created_at, updated_at, source_session, salience,"
                            "       provenance_category, use_cases"
                            "  FROM memories WHERE id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_fill_memory_12col_pg(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_touch(int64_t memory_id)
{
   if (memory_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Use the DB2 canonical UTC text format shared by the other timestamp
    * helpers so persisted stamps compare lexically and consistently. */
   static const char *sql = "UPDATE memories SET use_count = use_count + 1,"
                            " last_used_at = pg_now_text()"
                            " WHERE id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int changes = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes > 0 ? 0 : -1;
}

int db2_memory_update_content(int64_t memory_id, const char *content)
{
   if (memory_id <= 0 || !content || !content[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "UPDATE memories SET content = ?2, updated_at = pg_now_text()"
                            " WHERE id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", content);
   int changes = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_reject(int64_t memory_id, const char *reason)
{
   (void)reason;
   if (memory_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "UPDATE memories"
                            " SET confidence = GREATEST(confidence - 0.1, 0.0),"
                            "     updated_at = pg_now_text()"
                            " WHERE id = ?1";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int changes = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes > 0 ? 0 : -1;
}

int db2_memory_dedupe_by_key(int dry_run)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* libpq one-active-result-per-conn: the dedupe loop issues UPDATE
    * statements while iterating the SELECT, so materialize the (id, key)
    * pairs first, finalize, then walk the buffer issuing UPDATEs. */
   typedef struct
   {
      int64_t id;
      char key[512];
   } row_t;
   row_t *rows = NULL;
   int row_count = 0, row_cap = 0;

   static const char *find_sql = "SELECT id, key FROM memories"
                                 " WHERE merged_into = 0 AND tier NOT IN ('L0')"
                                 " ORDER BY key, confidence DESC";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *find = aimee_pg_prepare(conn, find_sql, err, sizeof(err));
   if (!find)
      return 0;
   while (aimee_pg_step(find, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *key = aimee_pg_column_text(find, 1);
      if (!key)
         continue;
      if (row_count == row_cap)
      {
         int new_cap = row_cap ? row_cap * 2 : 128;
         row_t *grown = (row_t *)realloc(rows, (size_t)new_cap * sizeof(row_t));
         if (!grown)
            break;
         rows = grown;
         row_cap = new_cap;
      }
      rows[row_count].id = aimee_pg_column_int64(find, 0);
      db2_copy_text(rows[row_count].key, sizeof(rows[row_count].key), key);
      row_count++;
   }
   aimee_pg_finalize(find);

   int merged = 0;
   char prev_key[512] = "";
   int64_t canonical_id = 0;

   for (int i = 0; i < row_count; i++)
   {
      int64_t id = rows[i].id;
      const char *key = rows[i].key;

      if (strcmp(key, prev_key) != 0)
      {
         snprintf(prev_key, sizeof(prev_key), "%s", key);
         canonical_id = id;
         continue;
      }
      if (id == canonical_id)
         continue;

      if (!dry_run)
      {
         static const char *upd_sql = "UPDATE memories SET merged_into = ?1 WHERE id = ?2";
         aimee_pg_stmt_t *upd = aimee_pg_prepare(conn, upd_sql, err, sizeof(err));
         if (upd)
         {
            aimee_pg_bind_int64(upd, "?1", canonical_id);
            aimee_pg_bind_int64(upd, "?2", id);
            (void)aimee_pg_step(upd, err, sizeof(err));
            aimee_pg_finalize(upd);
         }
      }
      merged++;
   }
   free(rows);
   return merged;
}

int db2_memory_episode_cards_query(const char *source_session, char **out, int max)
{
   if (!source_session || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT m.content FROM memories m"
                            "  JOIN memory_units u ON u.memory_id = m.id"
                            " WHERE u.is_episode_card = 1 AND m.source_session = ?1"
                            " ORDER BY m.id DESC LIMIT ?2";
   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", source_session);
   aimee_pg_bind_int(st, "?2", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ct = aimee_pg_column_text(st, 0);
      out[n++] = strdup(ct ? ct : "");
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_id_key_content(int limit, db2_memory_id_key_content_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT id, key, content FROM memories ORDER BY updated_at DESC%s",
            (limit > 0) ? " LIMIT ?1" : "");

   char err[MQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   if (limit > 0)
      aimee_pg_bind_int(st, "?1", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      db2_copy_text(out[n].key, sizeof(out[n].key), aimee_pg_column_text(st, 1));
      db2_copy_text(out[n].content, sizeof(out[n].content), aimee_pg_column_text(st, 2));
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
