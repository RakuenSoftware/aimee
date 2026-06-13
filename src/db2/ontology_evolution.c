/* ontology_evolution.c: self-extending ontology promotion pipeline (typed-fact
 * §2 / P4). See ontology_evolution.h. */
#include "ontology_evolution.h"
#include "../headers/rel_types.h" /* rel_type_normalize, REL_TYPE_NAME_MAX */
#include "rel_types_store.h"      /* db2_rel_types_resolve */
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define OE_ERRBUF 256

/* Host-clock UTC "now" in the stored text format (decision timestamps). */
static void oe_now_utc(char *buf, size_t n)
{
   time_t now_t = time(NULL);
   struct tm tmv;
   gmtime_r(&now_t, &tmv);
   strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

long db2_ontology_eval_observe(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   /* Insert or bump in one statement; RETURNING reads the new count. A bump only
    * touches occurrence_count — a previously rejected/mapped type keeps its
    * status (so it cannot silently re-surface as a candidate). */
   static const char *sql = "INSERT INTO ontology_evaluations (rel_type, occurrence_count, status)"
                            " VALUES (?1, 1, 'pending')"
                            " ON CONFLICT (rel_type) DO UPDATE"
                            " SET occurrence_count = ontology_evaluations.occurrence_count + 1"
                            " RETURNING occurrence_count";
   char err[OE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   long c = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      c = (long)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return c;
}

long db2_ontology_eval_count(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   char err[OE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT occurrence_count FROM ontology_evaluations WHERE rel_type = ?1 LIMIT 1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   long c = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      c = (long)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return c;
}

int db2_ontology_eval_status(const char *rel_type, char *out, size_t out_len)
{
   if (!rel_type || !rel_type[0] || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   char err[OE_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT status FROM ontology_evaluations WHERE rel_type = ?1 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", norm);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      snprintf(out, out_len, "%s", s ? s : "");
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

int db2_ontology_eval_candidates(int threshold, char (*out)[REL_TYPE_NAME_MAX], int max)
{
   if (threshold <= 0 || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[OE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT rel_type FROM ontology_evaluations"
                                          " WHERE status = 'pending' AND occurrence_count >= ?1"
                                          " ORDER BY occurrence_count DESC, rel_type ASC LIMIT ?2",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", threshold);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *nm = aimee_pg_column_text(st, 0);
      snprintf(out[n], REL_TYPE_NAME_MAX, "%s", nm ? nm : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* Run a single bound UPDATE inside the open transaction, returning rows changed
 * (or -1 on prepare/step failure). */
static int oe_update(void *conn, const char *sql, const char *p1, const char *p2, const char *p3)
{
   char err[OE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (p1)
      aimee_pg_bind_text(st, "?1", p1);
   if (p2)
      aimee_pg_bind_text(st, "?2", p2);
   if (p3)
      aimee_pg_bind_text(st, "?3", p3);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? changes : -1;
}

int db2_ontology_approve(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   char now_utc[32];
   oe_now_utc(now_utc, sizeof(now_utc));
   char err[OE_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   /* Promote the provisional rel_type to active (best-effort; a seeded type may
    * already be active) and mark the evaluation approved (must exist). */
   (void)oe_update(conn, "UPDATE rel_types SET status = 'active' WHERE rel_type = ?1", norm, NULL,
                   NULL);
   int ec = oe_update(
       conn,
       "UPDATE ontology_evaluations SET status = 'approved', decided_at = ?2 WHERE rel_type = ?1",
       norm, now_utc, NULL);
   if (ec != 1)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int db2_ontology_map(const char *novel, const char *target)
{
   if (!novel || !novel[0] || !target || !target[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char nn[REL_TYPE_NAME_MAX], tn[REL_TYPE_NAME_MAX];
   rel_type_normalize(novel, nn, sizeof(nn));
   rel_type_normalize(target, tn, sizeof(tn));
   if (!nn[0] || !tn[0] || strcmp(nn, tn) == 0)
      return -1;
   /* The map target must be a real rel_type. */
   long tid = 0;
   if (db2_rel_types_resolve(tn, &tid) != 1)
      return -1;
   char now_utc[32];
   oe_now_utc(now_utc, sizeof(now_utc));
   char err[OE_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   int ec = oe_update(conn,
                      "UPDATE ontology_evaluations SET status = 'mapped', mapped_to = ?2,"
                      " decided_at = ?3 WHERE rel_type = ?1",
                      nn, tn, now_utc);
   if (ec != 1)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   (void)oe_update(conn, "UPDATE rel_types SET status = 'mapped' WHERE rel_type = ?1", nn, NULL,
                   NULL);
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int db2_ontology_reject(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   if (!norm[0])
      return -1;
   char now_utc[32];
   oe_now_utc(now_utc, sizeof(now_utc));
   char err[OE_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   int ec = oe_update(
       conn,
       "UPDATE ontology_evaluations SET status = 'rejected', decided_at = ?2 WHERE rel_type = ?1",
       norm, now_utc, NULL);
   if (ec != 1)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   (void)oe_update(conn, "UPDATE rel_types SET status = 'rejected' WHERE rel_type = ?1", norm, NULL,
                   NULL);
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}
