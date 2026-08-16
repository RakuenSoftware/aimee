/* db2/memory_promotion.c: tier-promotion SQL primitives — Postgres via libpq. */

#include "memory_promotion.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MP_ERRBUF 256

int db2_memory_promotion_list_kinds_in_tier(const char *tier, db2_memory_promotion_kind_t *out,
                                            int max)
{
   if (!tier || !*tier || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT DISTINCT kind FROM memories WHERE tier = ?1";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", tier);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *k = aimee_pg_column_text(st, 0);
      if (!k)
         continue;
      snprintf(out[n].kind, sizeof(out[n].kind), "%s", k);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_promotion_promote_kind(const char *ts, const char *kind, int promote_use_count,
                                      double promote_confidence)
{
   if (!ts || !kind || !*kind)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE memories SET tier = 'L2', updated_at = ?1"
                            " WHERE tier = 'L1' AND kind = ?2"
                            "   AND (use_count >= ?3 OR confidence >= ?4)";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_int(st, "?3", promote_use_count);
   aimee_pg_bind_double(st, "?4", promote_confidence);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_promote_kind_slot(const char *ts, const char *kind, int promote_use_count,
                                           double promote_confidence, int slot_modulo,
                                           int slot_remainder, int slot_match)
{
   if (slot_modulo <= 0)
      return db2_memory_promotion_promote_kind(ts, kind, promote_use_count, promote_confidence);
   if (!ts || !kind || !*kind)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   const char *slot_filter = slot_match ? "   AND (id % ?5) = ?6" : "   AND (id % ?5) <> ?6";
   char sql[384];
   snprintf(sql, sizeof(sql),
            "UPDATE memories SET tier = 'L2', updated_at = ?1"
            " WHERE tier = 'L1' AND kind = ?2"
            "   AND (use_count >= ?3 OR confidence >= ?4)"
            "%s",
            slot_filter);

   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_int(st, "?3", promote_use_count);
   aimee_pg_bind_double(st, "?4", promote_confidence);
   aimee_pg_bind_int(st, "?5", slot_modulo);
   aimee_pg_bind_int(st, "?6", slot_remainder);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_demote_kind(const char *ts, const char *kind, double demote_confidence,
                                     const char *days_neg_str)
{
   if (!ts || !kind || !*kind || !days_neg_str)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE memories SET tier = 'L1', updated_at = ?1"
                            " WHERE tier = 'L2' AND kind = ?2"
                            "   AND confidence < ?3"
                            "   AND last_used_at < pg_now_text(?4 || ' days')";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_double(st, "?3", demote_confidence);
   aimee_pg_bind_text(st, "?4", days_neg_str);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_demote_cascade(const char *ts)
{
   if (!ts)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE memories SET confidence = confidence * 0.9"
                            " WHERE id IN ("
                            "  SELECT ml.source_id FROM memory_links ml"
                            "  JOIN memories m ON m.id = ml.target_id"
                            "  WHERE ml.relation = 'depends_on'"
                            "    AND m.tier = 'L1' AND m.updated_at = ?1"
                            ")";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_delete_l0_provenance(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[MP_ERRBUF] = "";
   int affected = 0;
   if (aimee_pg_exec_with_changes(conn,
                                  "DELETE FROM memory_provenance WHERE memory_id IN"
                                  " (SELECT id FROM memories WHERE tier = 'L0')",
                                  err, sizeof(err), &affected) != 0)
      return 0;
   return affected;
}

int db2_memory_promotion_delete_l0(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[MP_ERRBUF] = "";
   int affected = 0;
   if (aimee_pg_exec_with_changes(conn, "DELETE FROM memories WHERE tier = 'L0'", err, sizeof(err),
                                  &affected) != 0)
      return 0;
   return affected;
}

int db2_memory_promotion_delete_stale_l1_provenance(const char *kind, const char *days_neg_str)
{
   if (!kind || !*kind || !days_neg_str)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "DELETE FROM memory_provenance WHERE memory_id IN"
                            " (SELECT id FROM memories WHERE tier = 'L1' AND kind = ?1"
                            "   AND last_used_at < pg_now_text(?2 || ' days'))";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", kind);
   aimee_pg_bind_text(st, "?2", days_neg_str);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_delete_stale_l1(const char *kind, const char *days_neg_str)
{
   if (!kind || !*kind || !days_neg_str)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "DELETE FROM memories WHERE tier = 'L1' AND kind = ?1"
                            "  AND last_used_at < pg_now_text(?2 || ' days')";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", kind);
   aimee_pg_bind_text(st, "?2", days_neg_str);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_match_error_keys(const char *error_lowered, int64_t *ids_out, int max)
{
   if (!error_lowered || !*error_lowered || !ids_out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id FROM memories"
                            " WHERE tier IN ('L1', 'L2') AND confidence > 0.3"
                            "   AND ?1 LIKE '%' || LOWER(key) || '%'";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", error_lowered);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      ids_out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_promotion_demote_id(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "UPDATE memories SET confidence = confidence * 0.9, updated_at = pg_now_text()"
       " WHERE id = ?1 AND confidence > 0.3";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_list_unembedded_l2(const char *version, int64_t *ids_out, int max)
{
   if (!version || !*version || !ids_out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT m.id FROM memories m"
                            " WHERE m.tier = 'L2'"
                            " LIMIT ?1";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      ids_out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_memory_promotion_record_l4_approval(int64_t memory_id, const char *approver,
                                            const char *note)
{
   if (memory_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO memory_promotion_approvals"
                            "  (memory_id, target_tier, approver, note, approved_at)"
                            " VALUES (?1, 'L4', ?2, ?3, pg_now_text())"
                            " ON CONFLICT (memory_id, target_tier) DO UPDATE SET"
                            "   approver = excluded.approver,"
                            "   note = excluded.note,"
                            "   approved_at = excluded.approved_at";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", approver ? approver : "operator");
   aimee_pg_bind_text(st, "?3", note ? note : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_memory_promotion_reclassify_directives(int require_approval)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   const char *sql;
   if (require_approval)
   {
      sql = "UPDATE memories SET tier = 'L4'"
            " WHERE tier = 'L3'"
            "   AND (kind = 'workflow'"
            "        OR (kind = 'policy' AND id IN"
            "            (SELECT memory_id FROM memory_promotion_approvals"
            "             WHERE target_tier = 'L4')))";
   }
   else
   {
      sql = "UPDATE memories SET tier = 'L4'"
            " WHERE tier = 'L3'"
            "   AND kind IN ('policy', 'workflow')";
   }

   char err[MP_ERRBUF] = "";
   int affected = 0;
   if (aimee_pg_exec_with_changes(conn, sql, err, sizeof(err), &affected) != 0)
      return -1;
   return affected;
}

int db2_memory_promotion_promote_stable_l2_to_l3(const char *ts)
{
   if (!ts)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* "30 days since updated_at" expressed through the DB2 temporal helper so
    * the comparison keeps using the tier's canonical UTC text format. */
   static const char *sql = "UPDATE memories SET tier = 'L3', updated_at = ?1"
                            " WHERE tier = 'L2'"
                            "   AND kind IN ('fact', 'preference')"
                            "   AND confidence >= 0.95"
                            "   AND use_count >= 5"
                            "   AND updated_at <= pg_now_text('-30 days')";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changes;
}

int db2_memory_promotion_l5_pattern_candidates(db2_memory_l5_candidate_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* DB2 evaluates HAVING before SELECT aliases, so repeat the COUNT
    * expression there. */
   static const char *sql =
       "SELECT m.id, m.key, m.content, COUNT(DISTINCT p.session_id) AS session_count"
       " FROM memories m"
       " JOIN memory_provenance p ON p.memory_id = m.id"
       " WHERE m.tier = 'L2'"
       "   AND m.kind IN ('fact', 'pattern')"
       "   AND m.confidence >= 0.8"
       "   AND NOT EXISTS (SELECT 1 FROM memory_links ml"
       "                   JOIN memories ms ON ms.id = ml.source_id"
       "                   WHERE ml.target_id = m.id"
       "                     AND ms.tier = 'L5'"
       "                     AND ml.relation = 'synthesizes')"
       " GROUP BY m.id, m.key, m.content"
       " HAVING COUNT(DISTINCT p.session_id) >= 3"
       " LIMIT 20";
   char err[MP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].source_id = aimee_pg_column_int64(st, 0);
      const char *k = aimee_pg_column_text(st, 1);
      const char *c = aimee_pg_column_text(st, 2);
      snprintf(out[n].src_key, sizeof(out[n].src_key), "%s", k ? k : "");
      snprintf(out[n].src_content, sizeof(out[n].src_content), "%s", c ? c : "");
      out[n].session_count = aimee_pg_column_int(st, 3);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
