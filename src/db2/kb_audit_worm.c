/* kb_audit_worm.c: the aimee-kb per-service WORM audit store (S5). Postgres
 * (db2) append-only store with the SAME hash-chain record + canonicalization as
 * the aimee-server SQLite store (via audit_worm_chain), so a row hashed on either
 * engine verifies on the other. WORM is enforced by the kb_audit_event triggers
 * (schema.sql) + a writer role granted only INSERT/SELECT at provisioning. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audit_worm_chain.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "kb_audit_worm.h"

static void kb_worm_ts(char out[32])
{
   time_t now = time(NULL);
   struct tm tmv;
   gmtime_r(&now, &tmv);
   strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

int db2_kb_audit_append(const char *actor_role, const char *actor_principal, const char *action,
                        const char *subject, const char *verdict, const char *detail)
{
   if (!action || !action[0])
      return -1;
   actor_role = actor_role ? actor_role : "";
   actor_principal = actor_principal ? actor_principal : "";
   subject = subject ? subject : "";
   verdict = verdict ? verdict : "";
   detail = detail ? detail : "";

   /* Bound detail (R2-8), identical marker to the server store. */
   char detail_capped[AUDIT_WORM_DETAIL_MAX + 96];
   size_t dlen = strlen(detail);
   if (dlen > AUDIT_WORM_DETAIL_MAX)
   {
      snprintf(detail_capped, sizeof detail_capped, "%.*s\"[worm-truncated %zu bytes]\"",
               AUDIT_WORM_DETAIL_MAX, detail, dlen - (size_t)AUDIT_WORM_DETAIL_MAX);
      detail = detail_capped;
   }

   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256];
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof err) != 0)
      return -1;

   long long seq = 1;
   char prev[65];
   snprintf(prev, sizeof prev, "%s", AUDIT_WORM_GENESIS_PREV);
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn, "SELECT seq, row_hash FROM kb_audit_event ORDER BY seq DESC LIMIT 1", err, sizeof err);
   if (q)
   {
      if (aimee_pg_step(q, err, sizeof err) == AIMEE_PG_ROW)
      {
         seq = aimee_pg_column_int64(q, 0) + 1;
         const char *ph = aimee_pg_column_text(q, 1);
         if (ph)
            snprintf(prev, sizeof prev, "%s", ph);
      }
      aimee_pg_finalize(q);
   }

   char ts[32];
   kb_worm_ts(ts);
   char row_hash[65];
   audit_worm_row_hash(seq, actor_role, actor_principal, action, subject, verdict, "", detail, prev,
                       row_hash);

   aimee_pg_stmt_t *ins = aimee_pg_prepare(
       conn,
       "INSERT INTO kb_audit_event(seq, ts, actor_role, actor_principal, action, subject,"
       " verdict, detail, key_id, prev_hash, row_hash)"
       " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'',?9,?10)",
       err, sizeof err);
   if (!ins)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof err);
      return -1;
   }
   aimee_pg_bind_int64(ins, "?1", seq);
   aimee_pg_bind_text(ins, "?2", ts);
   aimee_pg_bind_text(ins, "?3", actor_role);
   aimee_pg_bind_text(ins, "?4", actor_principal);
   aimee_pg_bind_text(ins, "?5", action);
   aimee_pg_bind_text(ins, "?6", subject);
   aimee_pg_bind_text(ins, "?7", verdict);
   aimee_pg_bind_text(ins, "?8", detail);
   aimee_pg_bind_text(ins, "?9", prev);
   aimee_pg_bind_text(ins, "?10", row_hash);
   aimee_pg_step_t st = aimee_pg_step(ins, err, sizeof err);
   aimee_pg_finalize(ins);
   if (st == AIMEE_PG_ERR)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof err);
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof err) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof err);
      return -1;
   }
   return 0;
}

int db2_kb_audit_verify_chain(char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
   {
      if (err)
         snprintf(err, errlen, "no db2 connection");
      return -1;
   }
   char perr[256];
   aimee_pg_stmt_t *q = aimee_pg_prepare(
       conn,
       "SELECT seq, actor_role, actor_principal, action, subject, verdict, key_id, detail,"
       " prev_hash, row_hash FROM kb_audit_event ORDER BY seq ASC",
       perr, sizeof perr);
   if (!q)
   {
      if (err)
         snprintf(err, errlen, "query prepare failed: %s", perr);
      return -1;
   }
   int rc = 0;
   char prev[65];
   snprintf(prev, sizeof prev, "%s", AUDIT_WORM_GENESIS_PREV);
   long long expect = 1;
   while (aimee_pg_step(q, perr, sizeof perr) == AIMEE_PG_ROW)
   {
      long long seq = aimee_pg_column_int64(q, 0);
      const char *role = aimee_pg_column_text(q, 1);
      const char *principal = aimee_pg_column_text(q, 2);
      const char *action = aimee_pg_column_text(q, 3);
      const char *subject = aimee_pg_column_text(q, 4);
      const char *verdict = aimee_pg_column_text(q, 5);
      const char *key_id = aimee_pg_column_text(q, 6);
      const char *detail = aimee_pg_column_text(q, 7);
      const char *stored_prev = aimee_pg_column_text(q, 8);
      const char *stored_row = aimee_pg_column_text(q, 9);
      if (seq != expect)
      {
         if (err)
            snprintf(err, errlen, "seq gap: expected %lld, got %lld", expect, seq);
         rc = -1;
         break;
      }
      if (!stored_prev || strcmp(stored_prev, prev) != 0)
      {
         if (err)
            snprintf(err, errlen, "prev_hash break at seq %lld", seq);
         rc = -1;
         break;
      }
      char rh[65];
      audit_worm_row_hash(seq, role ? role : "", principal ? principal : "", action ? action : "",
                          subject ? subject : "", verdict ? verdict : "", key_id ? key_id : "",
                          detail ? detail : "", prev, rh);
      if (!stored_row || strcmp(rh, stored_row) != 0)
      {
         if (err)
            snprintf(err, errlen, "row_hash mismatch at seq %lld (tampered)", seq);
         rc = -1;
         break;
      }
      snprintf(prev, sizeof prev, "%s", stored_row);
      expect = seq + 1;
   }
   aimee_pg_finalize(q);
   return rc;
}

long db2_kb_audit_count(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[128];
   aimee_pg_stmt_t *q =
       aimee_pg_prepare(conn, "SELECT COUNT(*) FROM kb_audit_event", err, sizeof err);
   if (!q)
      return -1;
   long n = -1;
   if (aimee_pg_step(q, err, sizeof err) == AIMEE_PG_ROW)
      n = (long)aimee_pg_column_int64(q, 0);
   aimee_pg_finalize(q);
   return n;
}
