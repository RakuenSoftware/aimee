/* memory_lint.c: read-only memory consistency checks.
 *
 * Checks:
 *   orphan      — memories with no inbound or outbound links
 *   concept_gap — key repeated 3+ times but no concept memory for it
 *   stale_ref   — link targets with confidence < 0.2 */

#include "../headers/aimee.h"
#include "memory_lint.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ERRBUF 256

static int add_issue(memory_lint_issue_t *out, int count, int max, const char *type,
                     int64_t memory_id, const char *key, const char *msg)
{
   if (count >= max)
      return count;
   memory_lint_issue_t *iss = &out[count];
   snprintf(iss->type, sizeof(iss->type), "%s", type);
   iss->memory_id = memory_id;
   snprintf(iss->key, sizeof(iss->key), "%s", key ? key : "");
   snprintf(iss->message, sizeof(iss->message), "%s", msg ? msg : "");
   return count + 1;
}

static int check_orphans(void *conn, memory_lint_issue_t *out, int count, int max)
{
   char err[ERRBUF] = "";
   const char *sql = "SELECT id, key FROM memories "
                     "WHERE id NOT IN (SELECT source_id FROM memory_links) "
                     "  AND id NOT IN (SELECT target_id FROM memory_links) "
                     "  AND tier IN ('L1','L2','L3') "
                     "ORDER BY tier, key LIMIT 100";

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return count;

   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      int64_t id = aimee_pg_column_int64(st, 0);
      const char *key = aimee_pg_column_text(st, 1);
      char msg[MEMORY_LINT_MESSAGE_LEN];
      snprintf(msg, sizeof(msg), "no inbound or outbound links");
      count = add_issue(out, count, max, "orphan", id, key, msg);
   }
   aimee_pg_finalize(st);
   return count;
}

static int check_concept_gaps(void *conn, memory_lint_issue_t *out, int count, int max)
{
   char err[ERRBUF] = "";
   const char *sql = "SELECT key, COUNT(*) AS cnt FROM memories "
                     "WHERE kind != 'concept' "
                     "GROUP BY key HAVING COUNT(*) >= 3 "
                     "  AND NOT EXISTS ( "
                     "    SELECT 1 FROM memories m2 "
                     "    WHERE m2.key = memories.key AND m2.kind = 'concept' "
                     "  ) "
                     "ORDER BY cnt DESC LIMIT 20";

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return count;

   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      const char *key = aimee_pg_column_text(st, 0);
      int cnt = (int)aimee_pg_column_int64(st, 1);
      char msg[MEMORY_LINT_MESSAGE_LEN];
      snprintf(msg, sizeof(msg), "key appears %d times with no concept memory", cnt);
      count = add_issue(out, count, max, "concept_gap", 0, key, msg);
   }
   aimee_pg_finalize(st);
   return count;
}

static int check_stale_refs(void *conn, memory_lint_issue_t *out, int count, int max)
{
   char err[ERRBUF] = "";
   const char *sql = "SELECT ml.source_id, ms.key, mt.key AS target_key, mt.confidence "
                     "FROM memory_links ml "
                     "JOIN memories ms ON ms.id = ml.source_id "
                     "JOIN memories mt ON mt.id = ml.target_id "
                     "WHERE mt.confidence < 0.2 "
                     "ORDER BY mt.confidence LIMIT 50";

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return count;

   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      int64_t source_id = aimee_pg_column_int64(st, 0);
      const char *source_key = aimee_pg_column_text(st, 1);
      const char *target_key = aimee_pg_column_text(st, 2);
      double target_conf = aimee_pg_column_double(st, 3);
      char msg[MEMORY_LINT_MESSAGE_LEN];
      snprintf(msg, sizeof(msg), "links to low-confidence target '%s' (confidence=%.2f)",
               target_key ? target_key : "?", target_conf);
      count = add_issue(out, count, max, "stale_ref", source_id, source_key, msg);
   }
   aimee_pg_finalize(st);
   return count;
}

int memory_lint_run(memory_lint_issue_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return 0;

   int count = 0;
   count = check_orphans(conn, out, count, max);
   count = check_concept_gaps(conn, out, count, max);
   count = check_stale_refs(conn, out, count, max);
   return count;
}
