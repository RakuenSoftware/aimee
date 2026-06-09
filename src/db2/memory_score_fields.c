/* db2/memory_score_fields.c: tiny per-memory column accessors used by the
 * search-pipeline reranker. Split out of memory_query.c to keep that file
 * under the line-check budget; declarations still live in memory_query.h.
 *
 * Postgres via libpq. */

#include "../headers/aimee.h" /* memory_t for header consumers */
#include "db_postgres.h"
#include "memory_query.h"
#include "db2_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MSF_ERRBUF 256

int db2_memory_get_evidence_fields(int64_t memory_id, double *evidence, int *observations)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT evidence_strength, observation_count FROM memories WHERE id = ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (evidence)
         *evidence = aimee_pg_column_double(st, 0);
      if (observations)
         *observations = aimee_pg_column_int(st, 1);
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

double db2_memory_get_salience(int64_t memory_id, double default_value)
{
   return db2_scalar_double_id("SELECT salience FROM memories WHERE id = ?1", memory_id,
                               default_value);
}

double db2_memory_get_surprise(int64_t memory_id, double default_value)
{
   return db2_scalar_double_id("SELECT surprise FROM memories WHERE id = ?1", memory_id,
                               default_value);
}

int db2_memory_get_state_fields(int64_t memory_id, int *has_valid_until, int *observations,
                                int *use_count)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT valid_until, observation_count, use_count FROM memories WHERE id = ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (has_valid_until)
      {
         const char *vu = aimee_pg_column_text(st, 0);
         *has_valid_until = (vu && vu[0]) ? 1 : 0;
      }
      if (observations)
         *observations = aimee_pg_column_int(st, 1);
      if (use_count)
         *use_count = aimee_pg_column_int(st, 2);
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_get_confidence_by_key(const char *key, double *confidence_out)
{
   if (!key || !key[0] || !confidence_out)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT confidence FROM memories WHERE key = ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", key);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      *confidence_out = aimee_pg_column_double(st, 0);
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_list_by_key(const char *key, db2_memory_id_content_row_t *out, int max)
{
   if (!key || !key[0] || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id, content FROM memories WHERE key = ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", key);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(out[n].content, sizeof(out[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_session_l0_list(const char *session_id, db2_memory_session_l0_row_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT key, content FROM memories"
                            " WHERE tier = 'L0' AND source_session = ?1"
                            " ORDER BY created_at ASC";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *k = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(out[n].key, sizeof(out[n].key), "%s", k ? k : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_session_l0_purge(const char *session_id)
{
   if (!session_id || !session_id[0])
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[MSF_ERRBUF] = "";
   static const char *del_prov = "DELETE FROM memory_provenance WHERE memory_id IN"
                                 " (SELECT id FROM memories WHERE tier = 'L0'"
                                 "  AND source_session = ?1)";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, del_prov, err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", session_id);
      (void)aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }
   static const char *del_mem = "DELETE FROM memories WHERE tier = 'L0'"
                                " AND source_session = ?1";
   st = aimee_pg_prepare(conn, del_mem, err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", session_id);
      (void)aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }
}

int db2_memory_last_retro_scan(char *out, int out_len)
{
   if (!out || out_len <= 0)
      return 0;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT MAX(detected_at) FROM contradiction_log"
                            " WHERE details = 'retroactive_scan'";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0))
   {
      const char *t = aimee_pg_column_text(st, 0);
      if (t && t[0])
      {
         snprintf(out, (size_t)out_len, "%s", t);
         hit = 1;
      }
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_count_l2(void)
{
   return db2_scalar_int("SELECT COUNT(*) FROM memories WHERE tier = 'L2'", 0);
}

int db2_memory_count_l3(void)
{
   return db2_scalar_int("SELECT COUNT(*) FROM memories WHERE tier = 'L3'", 0);
}

int db2_memory_count_orphaned_l0(void)
{
   return db2_scalar_int("SELECT COUNT(*) FROM memories WHERE tier = 'L0'"
                         " AND created_at < pg_now_text('-7 days')",
                         0);
}

int db2_memory_prune_orphaned_l0(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "DELETE FROM memories WHERE tier = 'L0'"
                            " AND created_at < pg_now_text('-7 days')";
   char err[MSF_ERRBUF] = "";
   int affected = 0;
   if (aimee_pg_exec_with_changes(conn, sql, err, sizeof(err), &affected) != 0)
      return -1;
   return affected;
}

static int db2_memory_pair_scan(const char *sql, int max_pairs, db2_memory_pair_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max_pairs);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id_a = aimee_pg_column_int64(st, 0);
      out[n].id_b = aimee_pg_column_int64(st, 1);
      const char *ca = aimee_pg_column_text(st, 2);
      const char *cb = aimee_pg_column_text(st, 3);
      snprintf(out[n].content_a, sizeof(out[n].content_a), "%s", ca ? ca : "");
      snprintf(out[n].content_b, sizeof(out[n].content_b), "%s", cb ? cb : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_l2_cross_key_pairs(int max_pairs, db2_memory_pair_row_t *out, int max)
{
   /* INSTR(x,y) -> STRPOS(x,y); both 1-based, return 0 when not found. */
   static const char *sql = "SELECT a.id, b.id, a.content, b.content"
                            " FROM memories a, memories b"
                            " WHERE a.tier = 'L2' AND b.tier = 'L2'"
                            " AND a.id < b.id"
                            " AND a.key != b.key"
                            " AND a.confidence > 0.5 AND b.confidence > 0.5"
                            " AND ("
                            "   LOWER(a.key || ' ' || a.content) LIKE"
                            "     '%' || SUBSTR(b.key, 1, STRPOS(b.key || '_', '_') - 1) || '%'"
                            "   OR LOWER(b.key || ' ' || b.content) LIKE"
                            "     '%' || SUBSTR(a.key, 1, STRPOS(a.key || '_', '_') - 1) || '%'"
                            " )"
                            " LIMIT ?1";
   return db2_memory_pair_scan(sql, max_pairs, out, max);
}

int db2_memory_l2_fact_vs_decision_pairs(int max_pairs, db2_memory_pair_row_t *out, int max)
{
   static const char *sql = "SELECT f.id, d.id, f.content, d.content"
                            " FROM memories f, memories d"
                            " WHERE f.kind = 'fact' AND d.kind = 'decision'"
                            " AND f.tier IN ('L1', 'L2') AND d.tier IN ('L2', 'L3')"
                            " AND f.id != d.id"
                            " AND f.confidence > 0.5 AND d.confidence > 0.5"
                            " AND (LOWER(f.content) LIKE '%' || LOWER(d.key) || '%'"
                            "      OR LOWER(d.content) LIKE '%' || LOWER(f.key) || '%')"
                            " LIMIT ?1";
   return db2_memory_pair_scan(sql, max_pairs, out, max);
}

void db2_memory_record_retro_scan_marker(const char *ts)
{
   if (!ts || !ts[0])
      return;
   db2_exec_text("INSERT INTO contradiction_log"
                 " (detected_at, memory_a_id, memory_b_id, resolution, details)"
                 " VALUES (?1, NULL, NULL, 'scan', 'retroactive_scan')",
                 ts);
}

int db2_memory_session_id_content_list(const char *session_id, int limit,
                                       db2_memory_id_content_row_t *out, int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   if (limit <= 0 || limit > max)
      limit = max;
   static const char *sql = "SELECT id, content FROM memories WHERE source_session = ?1"
                            " ORDER BY id ASC LIMIT ?2";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);
   aimee_pg_bind_int(st, "?2", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(out[n].content, sizeof(out[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_set_source_session(int64_t memory_id, const char *session_id)
{
   if (memory_id <= 0 || !session_id)
      return;
   db2_exec_text_id("UPDATE memories SET source_session = ?1 WHERE id = ?2", session_id, memory_id);
}

void db2_memory_unit_episode_card_insert(int64_t memory_id, const char *unit_key,
                                         const char *unit_text)
{
   if (memory_id <= 0 || !unit_key || !unit_text)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   /* memory_units has UNIQUE(memory_id, unit_type, unit_key, unit_text). */
   static const char *sql =
       "INSERT INTO memory_units"
       "  (memory_id, unit_type, memory_kind, unit_key, unit_text, weight, is_episode_card)"
       " VALUES (?1, 'episode_card', 'episodic', ?2, ?3, 2.0, 1)"
       " ON CONFLICT (memory_id, unit_type, unit_key, unit_text) DO NOTHING";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", unit_key);
   aimee_pg_bind_text(st, "?3", unit_text);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int64_t db2_memory_unit_first_episode_card_id(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id FROM memory_units"
                            " WHERE memory_id = ?1 AND unit_type = 'episode_card' LIMIT 1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_memory_lookup_by_key(const char *key, int64_t *id_out, char *content_out, int content_len,
                             double *confidence_out, char *tier_out, int tier_len)
{
   if (!key || !key[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id, content, confidence, tier FROM memories WHERE key = ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", key);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (id_out)
         *id_out = aimee_pg_column_int64(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      if (content_out && content_len > 0)
         snprintf(content_out, (size_t)content_len, "%s", c ? c : "");
      if (confidence_out)
         *confidence_out = aimee_pg_column_double(st, 2);
      const char *t = aimee_pg_column_text(st, 3);
      if (tier_out && tier_len > 0)
         snprintf(tier_out, (size_t)tier_len, "%s", t ? t : "");
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_lookup_merge_fields(const char *key, int64_t *id_out, char *content_out,
                                   int content_len, double *confidence_out, int *use_count_out,
                                   double *surprise_out, int *observation_count_out,
                                   double *evidence_out)
{
   if (!key || !key[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id, content, confidence, use_count, surprise,"
                            " observation_count, evidence_strength FROM memories WHERE key = ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", key);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (id_out)
         *id_out = aimee_pg_column_int64(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      if (content_out && content_len > 0)
         snprintf(content_out, (size_t)content_len, "%s", c ? c : "");
      if (confidence_out)
         *confidence_out = aimee_pg_column_double(st, 2);
      if (use_count_out)
         *use_count_out = aimee_pg_column_int(st, 3);
      if (surprise_out)
         *surprise_out = aimee_pg_column_double(st, 4);
      if (observation_count_out)
         *observation_count_out = aimee_pg_column_int(st, 5);
      if (evidence_out)
         *evidence_out = aimee_pg_column_double(st, 6);
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_merge_update_ex(int64_t memory_id, const char *content, const char *use_cases,
                               double confidence, int use_count, int observation_count,
                               double evidence_strength, double salience, double surprise,
                               const char *ts)
{
   if (memory_id <= 0 || !content || !ts)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "UPDATE memories SET content = ?1, use_cases = ?2, confidence = ?3,"
                            " use_count = ?4, observation_count = ?5, evidence_strength = ?6,"
                            " salience = ?7, surprise = ?8, last_used_at = ?9, updated_at = ?10"
                            " WHERE id = ?11";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", content);
   aimee_pg_bind_text(st, "?2", use_cases ? use_cases : "");
   aimee_pg_bind_double(st, "?3", confidence);
   aimee_pg_bind_int(st, "?4", use_count);
   aimee_pg_bind_int(st, "?5", observation_count);
   aimee_pg_bind_double(st, "?6", evidence_strength);
   aimee_pg_bind_double(st, "?7", salience);
   aimee_pg_bind_double(st, "?8", surprise);
   aimee_pg_bind_text(st, "?9", ts);
   aimee_pg_bind_text(st, "?10", ts);
   aimee_pg_bind_int64(st, "?11", memory_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_memory_merge_update(int64_t memory_id, const char *content, double confidence,
                            int use_count, int observation_count, double evidence_strength,
                            double salience, double surprise, const char *ts)
{
   return db2_memory_merge_update_ex(memory_id, content, "", confidence, use_count,
                                     observation_count, evidence_strength, salience, surprise, ts);
}

int db2_memory_active_kind_dedupe_candidates(const char *kind, db2_memory_dedupe_candidate_t *out,
                                             int max)
{
   if (!kind || !kind[0] || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id, key, confidence, use_count, observation_count,"
                            " evidence_strength, surprise"
                            " FROM memories"
                            " WHERE kind = ?1 AND COALESCE(valid_until, '') = '' AND"
                            " key NOT LIKE '%#v%'"
                            " LIMIT 100";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", kind);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      snprintf(out[n].key, sizeof(out[n].key), "%s", k ? k : "");
      out[n].confidence = aimee_pg_column_double(st, 2);
      out[n].use_count = aimee_pg_column_int(st, 3);
      out[n].observation_count = aimee_pg_column_int(st, 4);
      out[n].evidence_strength = aimee_pg_column_double(st, 5);
      out[n].surprise = aimee_pg_column_double(st, 6);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int64_t db2_memory_row_insert_ex(const char *tier, const char *kind, const char *key,
                                 const char *content, const char *use_cases, double confidence,
                                 const char *session_id, const char *ts, const char *sensitivity,
                                 double evidence_strength, double salience, double surprise)
{
   if (!tier || !kind || !key || !ts)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* Postgres replaces sqlite's last_insert_rowid() with INSERT ... RETURNING id. */
   static const char *sql =
       "INSERT INTO memories (tier, kind, key, content, use_cases, confidence,"
       " use_count, last_used_at, source_session, created_at,"
       " updated_at, sensitivity, evidence_strength, salience, surprise, observation_count)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, 1, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, 1)"
       " RETURNING id";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", tier);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", key);
   aimee_pg_bind_text(st, "?4", content ? content : "");
   aimee_pg_bind_text(st, "?5", use_cases ? use_cases : "");
   aimee_pg_bind_double(st, "?6", confidence);
   aimee_pg_bind_text(st, "?7", ts);
   aimee_pg_bind_text(st, "?8", session_id ? session_id : "");
   aimee_pg_bind_text(st, "?9", ts);
   aimee_pg_bind_text(st, "?10", ts);
   aimee_pg_bind_text(st, "?11", sensitivity ? sensitivity : "");
   aimee_pg_bind_double(st, "?12", evidence_strength);
   aimee_pg_bind_double(st, "?13", salience);
   aimee_pg_bind_double(st, "?14", surprise);
   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return new_id;
}

int64_t db2_memory_row_insert(const char *tier, const char *kind, const char *key,
                              const char *content, double confidence, const char *session_id,
                              const char *ts, const char *sensitivity, double evidence_strength,
                              double salience, double surprise)
{
   return db2_memory_row_insert_ex(tier, kind, key, content, "", confidence, session_id, ts,
                                   sensitivity, evidence_strength, salience, surprise);
}

int db2_memory_count_versions(const char *key_prefix)
{
   if (!key_prefix)
      return 0;
   char like_pattern[560];
   snprintf(like_pattern, sizeof(like_pattern), "%s#v%%", key_prefix);
   return db2_scalar_int_text("SELECT COUNT(*) FROM memories WHERE key LIKE ?1", like_pattern, 0);
}

int db2_memory_set_versioned_key(int64_t memory_id, const char *versioned_key, const char *ts)
{
   if (memory_id <= 0 || !versioned_key || !ts)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "UPDATE memories SET key = ?1, valid_until = ?2, updated_at = ?3 WHERE id = ?4";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", versioned_key);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_text(st, "?3", ts);
   aimee_pg_bind_int64(st, "?4", memory_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

void db2_memory_set_valid_from(int64_t memory_id, const char *ts)
{
   if (memory_id <= 0 || !ts)
      return;
   db2_exec_text_id("UPDATE memories SET valid_from = ?1 WHERE id = ?2", ts, memory_id);
}

int db2_memory_list_low_effectiveness(double threshold, int limit, db2_memory_low_eff_row_t *rows,
                                      int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT id, tier, kind, key, effectiveness, use_count FROM memories"
                            " WHERE effectiveness IS NOT NULL AND effectiveness < ?1"
                            " ORDER BY effectiveness ASC LIMIT ?2";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_double(st, "?1", threshold);
   aimee_pg_bind_int(st, "?2", limit);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *tier = aimee_pg_column_text(st, 1);
      snprintf(rows[n].tier, sizeof(rows[n].tier), "%s", tier ? tier : "");
      const char *kind = aimee_pg_column_text(st, 2);
      snprintf(rows[n].kind, sizeof(rows[n].kind), "%s", kind ? kind : "");
      const char *key = aimee_pg_column_text(st, 3);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", key ? key : "");
      rows[n].effectiveness = aimee_pg_column_double(st, 4);
      rows[n].use_count = aimee_pg_column_int(st, 5);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_unused_l2(int days, db2_memory_unused_l2_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* days is an internal int caller-supplied, embedded in the modifier
    * string via snprintf. The datetime() shim accepts '-N days'. */
   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT id, key, tier, kind, confidence FROM memories"
            " WHERE tier = 'L2' AND use_count = 0"
            " AND created_at < pg_now_text('-%d days')"
            " ORDER BY created_at ASC LIMIT ?1",
            days > 0 ? days : 14);
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *key = aimee_pg_column_text(st, 1);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", key ? key : "");
      const char *tier = aimee_pg_column_text(st, 2);
      snprintf(rows[n].tier, sizeof(rows[n].tier), "%s", tier ? tier : "");
      const char *kind = aimee_pg_column_text(st, 3);
      snprintf(rows[n].kind, sizeof(rows[n].kind), "%s", kind ? kind : "");
      rows[n].confidence = aimee_pg_column_double(st, 4);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_superseded_keys(int min_versions, db2_memory_superseded_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Postgres rejects column aliases in HAVING (evaluated before SELECT);
    * repeat the COUNT(*) expression there. INSTR -> STRPOS. */
   static const char *sql =
       "SELECT SUBSTR(key, 1, STRPOS(key, '#v') - 1) AS base_key, COUNT(*) AS versions"
       " FROM memories WHERE key LIKE '%#v%'"
       " GROUP BY SUBSTR(key, 1, STRPOS(key, '#v') - 1)"
       " HAVING COUNT(*) >= ?1"
       " ORDER BY COUNT(*) DESC LIMIT ?2";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", min_versions);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *bk = aimee_pg_column_text(st, 0);
      snprintf(rows[n].base_key, sizeof(rows[n].base_key), "%s", bk ? bk : "");
      rows[n].versions = aimee_pg_column_int(st, 1);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_set_artifact(int64_t memory_id, const char *artifact_type, const char *artifact_ref,
                            const char *artifact_hash)
{
   if (memory_id <= 0 || !artifact_type || !artifact_ref)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "UPDATE memories SET artifact_type = ?1, artifact_ref = ?2,"
                            " artifact_hash = ?3 WHERE id = ?4";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", artifact_type);
   aimee_pg_bind_text(st, "?2", artifact_ref);
   if (artifact_hash && artifact_hash[0])
      aimee_pg_bind_text(st, "?3", artifact_hash);
   else
      aimee_pg_bind_null(st, "?3");
   aimee_pg_bind_int64(st, "?4", memory_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changed = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changed > 0 ? 1 : 0;
}

void db2_memory_set_cognified_kind(int64_t memory_id, const char *kind)
{
   if (memory_id <= 0 || !kind || !kind[0])
      return;
   db2_exec_text_id("UPDATE memories SET cognified_memory_kind = ?1 WHERE id = ?2", kind,
                    memory_id);
}

int db2_memory_get_content(int64_t memory_id, char *out, int out_len)
{
   if (!out || out_len <= 0)
      return 0;
   out[0] = '\0';
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT content FROM memories WHERE id = ?1 LIMIT 1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      snprintf(out, (size_t)out_len, "%s", c ? c : "");
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_summarise_clusters(double max_confidence, int min_count,
                                  db2_memory_summary_cluster_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Postgres rejects column aliases in HAVING; repeat COUNT(*) there. */
   static const char *sql =
       "SELECT source_session, COUNT(*) AS cnt, AVG(confidence) AS avg_conf"
       " FROM memories"
       " WHERE tier = 'L1' AND confidence <= ?1 AND kind = 'fact' AND merged_into = 0"
       " GROUP BY source_session"
       " HAVING COUNT(*) >= ?2";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_double(st, "?1", max_confidence);
   aimee_pg_bind_int(st, "?2", min_count);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *sid = aimee_pg_column_text(st, 0);
      if (!sid)
         continue;
      snprintf(rows[n].session_id, sizeof(rows[n].session_id), "%s", sid);
      rows[n].count = aimee_pg_column_int(st, 1);
      rows[n].avg_confidence = aimee_pg_column_double(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_mark_merged_into(int64_t merged_into, const char *session_id, double max_confidence)
{
   if (merged_into <= 0 || !session_id)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   static const char *sql = "UPDATE memories SET merged_into = ?1"
                            " WHERE tier = 'L1' AND confidence <= ?2 AND kind = 'fact'"
                            " AND source_session = ?3 AND merged_into = 0";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", merged_into);
   aimee_pg_bind_double(st, "?2", max_confidence);
   aimee_pg_bind_text(st, "?3", session_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_memory_list_artifact_hashed(db2_memory_artifact_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   int cap = max > 50 ? 50 : max;
   static const char *sql = "SELECT id, artifact_type, artifact_ref, artifact_hash"
                            " FROM memories WHERE artifact_type IS NOT NULL"
                            " AND artifact_hash IS NOT NULL LIMIT ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", cap);
   int n = 0;
   while (n < cap && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *t = aimee_pg_column_text(st, 1);
      snprintf(rows[n].artifact_type, sizeof(rows[n].artifact_type), "%s", t ? t : "");
      const char *r = aimee_pg_column_text(st, 2);
      snprintf(rows[n].artifact_ref, sizeof(rows[n].artifact_ref), "%s", r ? r : "");
      const char *h = aimee_pg_column_text(st, 3);
      snprintf(rows[n].artifact_hash, sizeof(rows[n].artifact_hash), "%s", h ? h : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_memory_decay_confidence(int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   db2_exec_id("UPDATE memories SET confidence = confidence * 0.7 WHERE id = ?1", memory_id);
}

int db2_memory_list_kv_section(db2_memory_section_t section, db2_memory_kv_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   const char *sql = NULL;
   switch (section)
   {
   case DB2_MEM_SECTION_ACTIVE_TASKS:
      sql = "SELECT key, content FROM memories"
            " WHERE (tier = 'L1' OR tier = 'L2') AND kind = 'task'"
            " ORDER BY updated_at DESC LIMIT ?1";
      break;
   case DB2_MEM_SECTION_RECENT_CONTEXT:
      sql = "SELECT key, content FROM memories"
            " WHERE tier = 'L1' AND kind = 'episode'"
            " ORDER BY created_at DESC LIMIT ?1";
      break;
   case DB2_MEM_SECTION_CONSTRAINTS:
      sql = "SELECT key, content FROM memories"
            " WHERE (tier = 'L2' OR tier = 'L3') AND (kind = 'decision' OR kind = 'policy')"
            " ORDER BY confidence DESC LIMIT ?1";
      break;
   case DB2_MEM_SECTION_PROCEDURES:
      sql = "SELECT key, content FROM memories"
            " WHERE (tier = 'L1' OR tier = 'L2') AND kind = 'procedure'"
            " ORDER BY confidence DESC, use_count DESC LIMIT ?1";
      break;
   case DB2_MEM_SECTION_FAILURE_WARNINGS:
      sql = "SELECT key, content FROM memories"
            " WHERE tier = 'L3' AND kind = 'episode' AND confidence > 0.3"
            " ORDER BY created_at DESC LIMIT ?1";
      break;
   default:
      return 0;
   }
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *k = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_episode_cards(db2_memory_episode_card_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   int cap = max > 4 ? 4 : max;
   static const char *sql = "SELECT m.content FROM memories m"
                            " JOIN memory_units u ON u.memory_id = m.id"
                            " WHERE u.is_episode_card = 1"
                            " ORDER BY m.confidence DESC, m.id DESC LIMIT ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", cap);
   int n = 0;
   while (n < cap && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_global_constraints(db2_memory_kv_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT key, content FROM memories"
                            " WHERE kind IN ('preference', 'policy')"
                            " AND tier IN ('L1', 'L2', 'L3', 'L4')"
                            " ORDER BY confidence DESC, use_count DESC LIMIT ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *k = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_id_kv_ws_section(db2_memory_ws_section_t section, db2_memory_id_kv_row_t *rows,
                                     int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   const char *sql = NULL;
   switch (section)
   {
   case DB2_MEM_WS_KEY_FACTS:
      sql = "SELECT m.id, m.key, m.content FROM memories m"
            " WHERE m.tier = 'L2' AND m.kind IN ('fact', 'preference')"
            " ORDER BY m.confidence DESC, m.use_count DESC LIMIT ?1";
      break;
   case DB2_MEM_WS_ACTIVE_TASKS:
      sql = "SELECT m.id, m.key, m.content FROM memories m"
            " WHERE (m.tier = 'L1' OR m.tier = 'L2') AND m.kind = 'task'"
            " ORDER BY m.updated_at DESC LIMIT ?1";
      break;
   case DB2_MEM_WS_RECENT_CONTEXT:
      sql = "SELECT m.id, m.key, m.content FROM memories m"
            " WHERE m.tier = 'L1' AND m.kind = 'episode'"
            " ORDER BY m.created_at DESC LIMIT ?1";
      break;
   case DB2_MEM_WS_CONSTRAINTS:
      sql = "SELECT m.id, m.key, m.content FROM memories m"
            " WHERE (m.tier = 'L2' OR m.tier = 'L3') AND m.kind IN ('decision', 'policy')"
            " ORDER BY m.confidence DESC LIMIT ?1";
      break;
   case DB2_MEM_WS_PROCEDURES:
      sql = "SELECT m.id, m.key, m.content FROM memories m"
            " WHERE (m.tier = 'L1' OR m.tier = 'L2') AND m.kind = 'procedure'"
            " ORDER BY m.confidence DESC, m.use_count DESC LIMIT ?1";
      break;
   case DB2_MEM_WS_CROSS_WORKSPACE:
      sql = "SELECT m.id, m.key, m.content FROM memories m"
            " WHERE m.tier = 'L2' AND m.confidence >= 0.9 AND m.use_count >= 5"
            " AND m.sensitivity IN ('public', 'normal')"
            " ORDER BY m.confidence DESC, m.use_count DESC LIMIT ?1";
      break;
   default:
      return 0;
   }
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      const char *c = aimee_pg_column_text(st, 2);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_count_and_max_updated(int *out_count, char *out_ts, int out_ts_len)
{
   if (!out_count || !out_ts || out_ts_len <= 0)
      return 0;
   *out_count = 0;
   out_ts[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT COUNT(*), MAX(updated_at) FROM memories";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      *out_count = aimee_pg_column_int(st, 0);
      const char *ts = aimee_pg_column_text(st, 1);
      snprintf(out_ts, (size_t)out_ts_len, "%s", ts ? ts : "");
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_supersede_lookup(int64_t new_memory_id, db2_memory_supersede_pair_t *out)
{
   if (!out || new_memory_id <= 0)
      return 0;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT old.id, old.content, COALESCE(old.valid_until, ''),"
                            " old.confidence, new.id, new.content, new.confidence"
                            " FROM memory_links ml"
                            " JOIN memories new ON new.id = ml.source_id"
                            " JOIN memories old ON old.id = ml.target_id"
                            " WHERE ml.relation = 'supersedes' AND new.id = ?1"
                            " LIMIT 1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", new_memory_id);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->old_id = aimee_pg_column_int64(st, 0);
      const char *oc = aimee_pg_column_text(st, 1);
      snprintf(out->old_content, sizeof(out->old_content), "%s", oc ? oc : "");
      const char *vu = aimee_pg_column_text(st, 2);
      snprintf(out->valid_until, sizeof(out->valid_until), "%s", vu ? vu : "");
      out->old_confidence = aimee_pg_column_double(st, 3);
      out->new_id = aimee_pg_column_int64(st, 4);
      const char *nc = aimee_pg_column_text(st, 5);
      snprintf(out->new_content, sizeof(out->new_content), "%s", nc ? nc : "");
      out->new_confidence = aimee_pg_column_double(st, 6);
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_memory_list_key_facts_with_provenance(db2_memory_key_fact_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT m.id, m.key, m.content,"
       " (SELECT MAX(p.created_at) FROM memory_provenance p"
       "  WHERE p.memory_id = m.id AND p.action = 'supersede') AS supersede_date,"
       " (SELECT p.action || ' during ' || p.session_id FROM memory_provenance p"
       "  WHERE p.memory_id = m.id ORDER BY p.created_at DESC LIMIT 1) AS provenance"
       " FROM memories m"
       " WHERE m.tier IN ('L2', 'L3') AND (m.kind = 'fact' OR m.kind = 'preference')"
       " ORDER BY m.confidence DESC, m.use_count DESC LIMIT ?1";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      const char *c = aimee_pg_column_text(st, 2);
      const char *sd = aimee_pg_column_text(st, 3);
      const char *pv = aimee_pg_column_text(st, 4);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      snprintf(rows[n].supersede_date, sizeof(rows[n].supersede_date), "%s", sd ? sd : "");
      snprintf(rows[n].provenance, sizeof(rows[n].provenance), "%s", pv ? pv : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_depends_on_keys(int64_t memory_id, db2_memory_key_row_t *rows, int max)
{
   if (!rows || max <= 0 || memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   int cap = max > 3 ? 3 : max;
   static const char *sql = "SELECT m.key FROM memory_links ml"
                            " JOIN memories m ON m.id = ml.target_id"
                            " WHERE ml.source_id = ?1 AND ml.relation = 'depends_on' LIMIT ?2";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_int(st, "?2", cap);
   int n = 0;
   while (n < cap && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *k = aimee_pg_column_text(st, 0);
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_candidates(db2_memory_cand_filter_t filter, db2_memory_cand_row_t *rows,
                               int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   const char *sql = NULL;
   switch (filter)
   {
   case DB2_MEM_CAND_PRIMARY:
      sql = "SELECT id, tier, key, content, kind, confidence, use_count FROM memories"
            " WHERE tier IN ('L1', 'L2', 'L3', 'L4', 'L5')"
            " ORDER BY confidence DESC, use_count DESC LIMIT ?1";
      break;
   case DB2_MEM_CAND_FALLBACK:
      sql = "SELECT id, tier, key, content, kind, confidence, use_count FROM memories"
            " WHERE tier IN ('L0', 'L1', 'L2', 'L4')"
            " ORDER BY confidence DESC, use_count DESC LIMIT ?1";
      break;
   default:
      return 0;
   }
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *t = aimee_pg_column_text(st, 1);
      const char *k = aimee_pg_column_text(st, 2);
      const char *c = aimee_pg_column_text(st, 3);
      const char *ki = aimee_pg_column_text(st, 4);
      snprintf(rows[n].tier, sizeof(rows[n].tier), "%s", t ? t : "");
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      snprintf(rows[n].kind, sizeof(rows[n].kind), "%s", ki ? ki : "");
      rows[n].confidence = aimee_pg_column_double(st, 5);
      rows[n].use_count = aimee_pg_column_int(st, 6);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_l1_session_clusters(const char *excluded_source, int min_count,
                                   db2_memory_l1_cluster_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT source_session, COUNT(*)"
                            " FROM memories"
                            " WHERE tier = 'L1' AND source_session != ''"
                            " AND source_session != ?1"
                            " GROUP BY source_session"
                            " HAVING COUNT(*) >= ?2";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", excluded_source ? excluded_source : "");
   aimee_pg_bind_int(st, "?2", min_count);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *sid = aimee_pg_column_text(st, 0);
      if (!sid)
         continue;
      snprintf(rows[n].session_id, sizeof(rows[n].session_id), "%s", sid);
      rows[n].count = aimee_pg_column_int(st, 1);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_l1_session_created_at(const char *session_id, db2_memory_created_at_row_t *rows,
                                     int max)
{
   if (!rows || max <= 0 || !session_id)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT created_at FROM memories"
                            " WHERE tier = 'L1' AND source_session = ?1"
                            " ORDER BY created_at ASC";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *ca = aimee_pg_column_text(st, 0);
      snprintf(rows[n].created_at, sizeof(rows[n].created_at), "%s", ca ? ca : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_l1_session_content(const char *session_id, db2_memory_content_row_t *rows, int max)
{
   if (!rows || max <= 0 || !session_id)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT content FROM memories"
                            " WHERE tier = 'L1' AND source_session = ?1"
                            " ORDER BY created_at ASC";
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", session_id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_list_recall_section(db2_memory_recall_section_t section, db2_memory_cand_row_t *rows,
                                   int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   const char *sql = NULL;
   switch (section)
   {
   case DB2_MEM_RECALL_IDENTITY:
      sql = "SELECT id, tier, kind, key, content FROM memories"
            " WHERE tier IN ('L2','L3','L4','L5')"
            " AND kind = 'fact'"
            " AND lifecycle_state != 'archived' AND lifecycle_state != 'superseded'"
            " AND (key LIKE 'identity:%' OR key LIKE 'name:%' OR key LIKE 'role:%'"
            "      OR key LIKE 'user:%' OR key LIKE 'self:%')"
            " ORDER BY confidence DESC, evidence_strength DESC, id DESC LIMIT ?1";
      break;
   case DB2_MEM_RECALL_PREFERENCES:
      sql = "SELECT id, tier, kind, key, content FROM memories"
            " WHERE tier IN ('L2','L3','L4','L5')"
            " AND kind = 'preference'"
            " AND lifecycle_state != 'archived' AND lifecycle_state != 'superseded'"
            " ORDER BY confidence DESC, observation_count DESC, id DESC LIMIT ?1";
      break;
   case DB2_MEM_RECALL_ACTIVE_CONTEXT:
      sql = "SELECT id, tier, kind, key, content FROM memories"
            " WHERE tier IN ('L1','L2','L3','L4')"
            " AND kind IN ('fact','decision','policy','task')"
            " AND lifecycle_state != 'archived' AND lifecycle_state != 'superseded'"
            " AND COALESCE(last_used_at, updated_at) >= pg_now_text('-7 days')"
            " ORDER BY COALESCE(last_used_at, updated_at) DESC, id DESC LIMIT ?1";
      break;
   case DB2_MEM_RECALL_OPEN_COMMITMENTS:
      sql = "SELECT id, tier, kind, key, content FROM memories"
            " WHERE lifecycle_state = 'pending'"
            " ORDER BY COALESCE(ttl_at, created_at) ASC, id ASC LIMIT ?1";
      break;
   default:
      return 0;
   }
   char err[MSF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&rows[n], 0, sizeof(rows[n]));
      rows[n].id = aimee_pg_column_int64(st, 0);
      const char *t = aimee_pg_column_text(st, 1);
      const char *ki = aimee_pg_column_text(st, 2);
      const char *k = aimee_pg_column_text(st, 3);
      const char *c = aimee_pg_column_text(st, 4);
      snprintf(rows[n].tier, sizeof(rows[n].tier), "%s", t ? t : "");
      snprintf(rows[n].kind, sizeof(rows[n].kind), "%s", ki ? ki : "");
      snprintf(rows[n].key, sizeof(rows[n].key), "%s", k ? k : "");
      snprintf(rows[n].content, sizeof(rows[n].content), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_count_l2_for_session(const char *source_session)
{
   if (!source_session || !source_session[0])
      return 0;
   return db2_scalar_int_text(
       "SELECT COUNT(*) FROM memories WHERE tier = 'L2' AND source_session = ?1", source_session,
       0);
}
