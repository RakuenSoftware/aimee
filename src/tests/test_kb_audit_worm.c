/* test_kb_audit_worm.c: the KB producer contract against the DB2 SQLite shim.
 * The producer owns an immutable outbox only; the separately tested worker
 * writes the shared audit_worm SQLite chain. */
#include "../modules/db2/c/db2_test_shim.h"
#include "artifacts.h"
#include "modules/db2/c/db2_internal.h"
#include "db_postgres.h"
#include "kb_audit_worm.h"

#include <assert.h>
#include <stdio.h>

static long long scalar(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   long long value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static long long outbox_count(void)
{
   return scalar("SELECT COUNT(*) FROM kb_audit_outbox");
}

static void test_submit_and_pending(void)
{
   long long before = outbox_count();
   assert(db2_kb_audit_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(db2_kb_audit_append("delegate", "mimo", "kb.query", "q1", "ok", "{}") == 0);
   assert(outbox_count() == before + 2);
   long long pending = -1, age = -1;
   assert(db2_kb_audit_pending(&pending, &age) == 0);
   assert(pending == before + 2);
   assert(age >= 0);
   printf("  PASS: kb_submit_and_pending\n");
}

static void test_transactional_submit(void)
{
   char err[256] = "";
   long long before = outbox_count();
   assert(aimee_pg_exec(db2_conn(), "BEGIN", err, sizeof(err)) == 0);
   assert(db2_kb_audit_append_in_txn(db2_conn(), "primary", "u", "fact.write", "f1", "ok", "{}") ==
          0);
   assert(aimee_pg_exec(db2_conn(), "ROLLBACK", err, sizeof(err)) == 0);
   assert(outbox_count() == before);
   printf("  PASS: kb_transactional_submit\n");
}

static void test_outbox_is_immutable(void)
{
   char err[256] = "";
   assert(db2_kb_audit_append("primary", "u", "tool.read", "immutable", "allow", "{}") == 0);
   assert(aimee_pg_exec(db2_conn(),
                        "UPDATE kb_audit_outbox SET verdict='block' WHERE subject='immutable'", err,
                        sizeof(err)) != 0);
   assert(aimee_pg_exec(db2_conn(), "DELETE FROM kb_audit_outbox WHERE subject='immutable'", err,
                        sizeof(err)) != 0);
   printf("  PASS: kb_outbox_is_immutable\n");
}

static void test_capture_via_seam(void)
{
   assert(db2_artifact_write("src-cap", "doc", "proposed", "entity", "", "curator", 1.0, "{}") ==
          0);
   assert(db2_artifact_write("src-cap2", "doc", "proposed", "user", "", "user", 1.0, "{}") == 0);

   db2_kb_audit_worm_set_enabled(1);
   long long before = outbox_count();
   assert(db2_audit_event_write("aid-cap", "src-cap", "kb.curator.promote_entity", "tgt-cap",
                                "curator", "entity", "", 1.0, 0, NULL, "{}") == 0);
   assert(outbox_count() == before + 1);

   db2_kb_audit_worm_set_enabled(0);
   before = outbox_count();
   assert(db2_audit_event_write("aid-cap2", "src-cap2", "docs", "tgt2", "user", "user", "", 1.0, 0,
                                NULL, "{}") == 0);
   assert(outbox_count() == before);
   db2_kb_audit_worm_set_enabled(1);
   printf("  PASS: kb_capture_via_seam\n");
}

int main(void)
{
   db2_test_shim_open();
   test_submit_and_pending();
   test_transactional_submit();
   test_outbox_is_immutable();
   test_capture_via_seam();
   db2_test_shim_close();
   printf("kb_audit_worm: all tests passed\n");
   return 0;
}
