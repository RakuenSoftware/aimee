/* test_code_index_ops.c: code-chunk replay bookkeeping over the sqlite shim. */
#include <assert.h>
#include <stdio.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/code_index_ops.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"

int main(void)
{
   db2_test_shim_open();

   db2_code_index_ops_summary_t sum;
   /* ok embed recorded */
   db2_code_index_op_record(1, "proj", "file:src/a.c", "src/a.c", 1, NULL);
   /* a failing embed, recorded twice → attempts climbs */
   db2_code_index_op_record(2, "proj", "file:src/b.c", "src/b.c", 0, "boom");
   db2_code_index_op_record(2, "proj", "file:src/b.c", "src/b.c", 0, "boom");

   assert(db2_code_index_ops_summary(2, &sum) == 0);
   assert(sum.ok_ops == 1);
   assert(sum.failed_ops == 1);
   assert(sum.stuck_ops == 1); /* point 2 has attempts >= 2 */
   printf("  record ok/failed + summary OK (ok=%lld failed=%lld stuck=%lld)\n",
          (long long)sum.ok_ops, (long long)sum.failed_ops, (long long)sum.stuck_ops);

   /* reset-stuck clears the stuck failed row's attempts */
   int reset = db2_code_index_ops_reset_stuck(2);
   assert(reset == 1);
   assert(db2_code_index_ops_summary(2, &sum) == 0);
   assert(sum.stuck_ops == 0);  /* no longer stuck */
   assert(sum.failed_ops == 1); /* still failed, but retryable */
   printf("  reset-stuck retries a stuck code embed OK\n");

   /* D7 drift detector: a code embedding whose source file was re-scanned after
    * the embedding was written is a re-ingest candidate. Crucially this exercises
    * the timestamp-format normalization: files.scanned_at is now_utc() ('...T..Z')
    * while code_embeddings.updated_at is space-separated, so a raw compare would
    * mis-order — the detector must normalize before comparing. */
   {
      void *conn = db2_conn();
      char e[256] = "";
      assert(aimee_pg_exec(
                 conn, "INSERT INTO projects (name, root, scanned_at) VALUES ('dproj','/x','x')", e,
                 sizeof e) == 0);
      /* stale: file re-scanned (2026-06-02, T/Z form) AFTER the embed (2026-06-01, space form) */
      assert(aimee_pg_exec(conn,
                           "INSERT INTO files (project_id, path, hash, scanned_at) VALUES"
                           " ((SELECT id FROM projects WHERE name='dproj'),"
                           " 'src/stale.c','h','2026-06-02T00:00:00Z')",
                           e, sizeof e) == 0);
      assert(aimee_pg_exec(
                 conn,
                 "INSERT INTO code_embeddings (point_id, project, file_path, node_key,"
                 " updated_at) VALUES (101,'dproj','src/stale.c','n1','2026-06-01 00:00:00')",
                 e, sizeof e) == 0);
      /* fresh: file scanned (2026-06-01) BEFORE the embed (2026-06-02) -> not a candidate */
      assert(aimee_pg_exec(conn,
                           "INSERT INTO files (project_id, path, hash, scanned_at) VALUES"
                           " ((SELECT id FROM projects WHERE name='dproj'),"
                           " 'src/fresh.c','h','2026-06-01T00:00:00Z')",
                           e, sizeof e) == 0);
      assert(aimee_pg_exec(
                 conn,
                 "INSERT INTO code_embeddings (point_id, project, file_path, node_key,"
                 " updated_at) VALUES (102,'dproj','src/fresh.c','n2','2026-06-02 00:00:00')",
                 e, sizeof e) == 0);
      int64_t drift = db2_code_index_drift_candidates();
      assert(drift == 1); /* only src/stale.c — format normalization makes the compare correct */
      printf("  drift detector flags re-scanned-since-embed (got %lld) OK\n", (long long)drift);
   }

   db2_test_shim_close();
   printf("code_index_ops: all tests passed\n");
   return 0;
}
