/* Real-PostgreSQL proof for the KB side of the SQLite WORM bridge. Producers
 * submit immutable intents; the worker API claims and acknowledges delivery
 * without constructing or storing a PostgreSQL hash chain. */
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "kb_audit_worm.h"
#include "config_embedder_dims.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static long long scalar(void *conn, const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   assert(st);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   long long value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static int sql_submit(void *conn)
{
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT kb_audit_worm_submit('primary','u','p2a.test','s','ok','{}')",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_ROW ? 0 : -1;
}

static void run(void)
{
   void *conn = db2_conn();
   assert(conn);
   long long outbox_before = scalar(conn, "SELECT COUNT(*) FROM kb_audit_outbox");
   long long delivered_before = scalar(conn, "SELECT COUNT(*) FROM kb_audit_delivery");

   assert(db2_kb_audit_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(sql_submit(conn) == 0);
   assert(db2_kb_audit_append("delegate", "mimo", "kb.query", "q1", "ok", "{}") == 0);
   assert(scalar(conn, "SELECT COUNT(*) FROM kb_audit_outbox") == outbox_before + 3);

   char err[256] = "";
   assert(aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) == 0);
   aimee_pg_stmt_t *claim =
       aimee_pg_prepare(conn, "SELECT outbox_id FROM kb_audit_worm_claim(1000)", err, sizeof(err));
   assert(claim);
   long long ids[1000];
   int count = 0;
   while (count < 1000 && aimee_pg_step(claim, err, sizeof(err)) == AIMEE_PG_ROW)
      ids[count++] = aimee_pg_column_int64(claim, 0);
   aimee_pg_finalize(claim);
   assert(count >= 3);

   long long next_seq = scalar(conn, "SELECT COALESCE(MAX(audit_seq),0) FROM kb_audit_delivery");
   for (int i = 0; i < count; ++i)
   {
      aimee_pg_stmt_t *ack =
          aimee_pg_prepare(conn, "SELECT kb_audit_worm_ack(?1,?2)", err, sizeof(err));
      assert(ack);
      aimee_pg_bind_int64(ack, "?1", ids[i]);
      aimee_pg_bind_int64(ack, "?2", ++next_seq);
      assert(aimee_pg_step(ack, err, sizeof(err)) == AIMEE_PG_ROW);
      aimee_pg_finalize(ack);
   }
   assert(aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0);
   assert(scalar(conn, "SELECT COUNT(*) FROM kb_audit_delivery") == delivered_before + count);
   long long pending = -1, age = -1;
   assert(db2_kb_audit_pending(&pending, &age) == 0);
   assert(pending == 0);
   printf("  PASS: mixed producers submit and claim/ack delivers %d intents\n", count);
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("kb_audit_worm_pg: SKIP (AIMEE_TEST_PG_URL unset; real Postgres required)\n");
      return 0;
   }
   db2_set_embedding_dim_default(CONFIG_EMBEDDER_DIMS_DEFAULT);
   db2_set_embedding_dim(CONFIG_EMBEDDER_DIMS_DEFAULT);
   if (db2_init(url) != 0)
   {
      fputs("kb_audit_worm_pg: db2_init failed\n", stderr);
      return 1;
   }
   run();
   db2_shutdown();
   printf("kb_audit_worm_pg: all tests passed\n");
   return 0;
}
