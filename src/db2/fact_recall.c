/* fact_recall.c: typed-fact recall into the envelope + §7 PII gating. P5.
 * See fact_recall.h. */
#include "fact_recall.h"
#include "../headers/memory_pii_gate.h" /* memory_pii_rel_sensitivity / should_inject */
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define FR_ERRBUF    256
#define FR_MAX_FACTS 32

int db2_fact_recall_block(const char *entity, int turn_requests_sensitive, char *out, size_t cap)
{
   if (!entity || !entity[0] || !out || cap == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Current facts only: active (superseded_at='') and not tombstoned
    * (suppressed=0). Highest confidence first so the most reliable facts win the
    * budget. Source-only (the entity is the subject of "my X is Y"-style facts). */
   static const char *sql = "SELECT relation, target, confidence FROM entity_edges"
                            " WHERE source = ?1 AND edge_class = 'semantic'"
                            "   AND superseded_at = '' AND suppressed = 0"
                            " ORDER BY confidence DESC, id ASC LIMIT ?2";
   char err[FR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_int(st, "?2", FR_MAX_FACTS);

   int written = 0;
   size_t used = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *rel = aimee_pg_column_text(st, 0);
      const char *tgt = aimee_pg_column_text(st, 1);
      double conf = aimee_pg_column_double(st, 2);
      if (!rel || !rel[0] || !tgt || !tgt[0])
         continue;
      /* §7 PII gate: sensitivity comes from the rel_type (fail-closed to PII for
       * unknown types); withhold unless the turn asks. */
      rel_sensitivity_t sens = memory_pii_rel_sensitivity(rel);
      if (!memory_pii_should_inject(sens, conf, turn_requests_sensitive))
         continue;
      char line[256];
      int n = snprintf(line, sizeof(line), "- %s: %s\n", rel, tgt);
      /* Skip empty or over-long lines: snprintf returns the would-be length, so a
       * line >= sizeof(line) was truncated — never memcpy that length (it would
       * over-read the stack buffer) and never inject a truncated fact. */
      if (n <= 0 || (size_t)n >= sizeof(line))
         continue;
      if (used + (size_t)n >= cap) /* respect the caller's buffer */
         break;
      memcpy(out + used, line, (size_t)n);
      used += (size_t)n;
      out[used] = '\0';
      written++;
   }
   aimee_pg_finalize(st);
   return written;
}

#define FR_MAX_ENTITIES 8

int db2_fact_recall_in_query(const char *query, int turn_requests_sensitive, char *out, size_t cap)
{
   if (!query || !out || cap == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* The user's own facts first. */
   int total = db2_fact_recall_block("user", turn_requests_sensitive, out, cap);
   if (total < 0)
      total = 0;
   size_t used = strlen(out);

   /* Entities mentioned in the query: any active entity whose alias (>=3 chars,
    * to avoid noise) is a substring of the lowercased query. LIKE + || is
    * portable across Postgres and the sqlite shim (position()/instr() are not).
    * Skip "user" (already done above). Return each entity's preferred name. */
   static const char *sql =
       "SELECT (SELECT name FROM entity_aliases p WHERE p.canonical_id = r.canonical_id"
       "          AND p.suppressed = 0 ORDER BY is_preferred DESC, id ASC LIMIT 1) AS pref"
       " FROM entity_registry r"
       " WHERE r.status = 'active'"
       "   AND EXISTS (SELECT 1 FROM entity_aliases a WHERE a.canonical_id = r.canonical_id"
       "                 AND a.suppressed = 0 AND length(a.name_norm) >= 3"
       "                 AND lower(?1) LIKE '%' || a.name_norm || '%')"
       " LIMIT ?2";
   char err[FR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return total;
   aimee_pg_bind_text(st, "?1", query);
   aimee_pg_bind_int(st, "?2", FR_MAX_ENTITIES);

   char names[FR_MAX_ENTITIES][128];
   int nnames = 0;
   while (nnames < FR_MAX_ENTITIES && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *pref = aimee_pg_column_text(st, 0);
      if (pref && pref[0] && strcmp(pref, "user") != 0)
         snprintf(names[nnames++], 128, "%s", pref);
   }
   aimee_pg_finalize(st);

   /* Recall each mentioned entity's facts into the remaining buffer. */
   for (int i = 0; i < nnames && used + 1 < cap; i++)
   {
      int en = db2_fact_recall_block(names[i], turn_requests_sensitive, out + used, cap - used);
      if (en > 0)
      {
         total += en;
         used = strlen(out);
      }
   }
   return total;
}
