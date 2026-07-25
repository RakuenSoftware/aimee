/* kb_curator_contradictions.c: deep-curator detect_contradictions pass.
 *
 * Self-joins curator_claim_vectors to find claim pairs that share a
 * subject+attribute but assert different values, and that are not already
 * linked, then writes a `contradicts` artifact_link between the two claim
 * artifacts. This is the deterministic exact-facet pass; fuzzy
 * subj_attr-similar / value-different mining over the named vectors is a
 * refinement. db2_artifact_link is idempotent, so re-running is safe.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_contradictions.h"
#include "aimee.h"
#include "log.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <string.h>

/* Max claim pairs linked per drain call (bounds work per poll). */
#define CONTRA_BATCH 32

int kb_curator_detect_contradictions_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* Pairs with a shared subject+attribute but a different value that are not
    * already linked as contradictions. a.point_id < b.point_id keeps each
    * unordered pair once. */
   static const char *sql =
       "SELECT a.artifact_id, b.artifact_id"
       " FROM curator_claim_vectors a"
       " JOIN curator_claim_vectors b"
       "   ON a.subject = b.subject AND a.attribute = b.attribute"
       "  AND a.subject <> '' AND a.value <> b.value AND a.point_id < b.point_id"
       " WHERE NOT EXISTS ("
       "   SELECT 1 FROM artifact_links l"
       "    WHERE l.from_id = a.artifact_id AND l.to_id = b.artifact_id"
       "      AND l.kind = 'contradicts')"
       " LIMIT 32";

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char from_ids[CONTRA_BATCH][64];
   char to_ids[CONTRA_BATCH][64];
   int n = 0;
   while (n < CONTRA_BATCH && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *a = aimee_pg_column_text(st, 0);
      const char *b = aimee_pg_column_text(st, 1);
      snprintf(from_ids[n], sizeof(from_ids[n]), "%s", a ? a : "");
      snprintf(to_ids[n], sizeof(to_ids[n]), "%s", b ? b : "");
      n++;
   }
   aimee_pg_finalize(st);

   int written = 0;
   for (int i = 0; i < n; i++)
   {
      if (!from_ids[i][0] || !to_ids[i][0])
         continue;
      if (db2_artifact_link(from_ids[i], to_ids[i], "contradicts") == 0)
         written++;
   }
   if (written > 0)
      aimee_log(LOG_INFO, "kb.curator.contradict", "linked %d contradicting claim pair(s)",
                written);
   return written > 0 ? 1 : 0;
}
