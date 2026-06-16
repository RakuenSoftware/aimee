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

   /* D7 drift detector — covers both predicate branches:
    *  - LEGACY staleness fallback (source_hash=''): src/stale.c (re-scanned after
    *    embed) flags; src/fresh.c does not. Exercises the timestamp-format
    *    normalization (files.scanned_at is now_utc() '...T..Z' vs space-separated
    *    code_embeddings.updated_at — a raw compare would mis-order).
    *  - PRECISE content hash (source_hash<>''): src/changed.c flags because
    *    files.hash differs from the stored source_hash EVEN THOUGH it was scanned
    *    BEFORE the embed (staleness alone would miss it); src/same.c does NOT flag
    *    because the hashes match EVEN THOUGH it was re-scanned after the embed
    *    (staleness alone would false-positive). */
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
      /* precise: file hash CHANGED (hNEW) vs stored source_hash (hOLD), but the
       * file was scanned (06-01) BEFORE the embed (06-02) — staleness alone misses
       * it; the hash branch flags it. */
      assert(aimee_pg_exec(conn,
                           "INSERT INTO files (project_id, path, hash, scanned_at) VALUES"
                           " ((SELECT id FROM projects WHERE name='dproj'),"
                           " 'src/changed.c','hNEW','2026-06-01T00:00:00Z')",
                           e, sizeof e) == 0);
      assert(aimee_pg_exec(conn,
                           "INSERT INTO code_embeddings (point_id, project, file_path, node_key,"
                           " source_hash, updated_at) VALUES"
                           " (103,'dproj','src/changed.c','n3','hOLD','2026-06-02 00:00:00')",
                           e, sizeof e) == 0);
      /* precise no-false-positive: hashes MATCH (hX), but the file was re-scanned
       * (06-02) AFTER the embed (06-01) — staleness alone would flag it; the hash
       * branch correctly does not. */
      assert(aimee_pg_exec(conn,
                           "INSERT INTO files (project_id, path, hash, scanned_at) VALUES"
                           " ((SELECT id FROM projects WHERE name='dproj'),"
                           " 'src/same.c','hX','2026-06-02T00:00:00Z')",
                           e, sizeof e) == 0);
      assert(aimee_pg_exec(conn,
                           "INSERT INTO code_embeddings (point_id, project, file_path, node_key,"
                           " source_hash, updated_at) VALUES"
                           " (104,'dproj','src/same.c','n4','hX','2026-06-01 00:00:00')",
                           e, sizeof e) == 0);
      int64_t drift = db2_code_index_drift_candidates();
      assert(drift == 2); /* src/stale.c (staleness) + src/changed.c (hash); fresh+same excluded */
      printf("  drift detector flags staleness + precise hash drift (got %lld) OK\n",
             (long long)drift);

      /* D7 requeue: the one drifted project ('dproj') gets enqueued for re-ingest
       * with force, deduped — a second call enqueues nothing (already pending). */
      int q1 = db2_code_index_requeue_drifted();
      assert(q1 == 1);
      aimee_pg_stmt_t *qs = aimee_pg_prepare(conn,
                                             "SELECT COUNT(*), MAX(force) FROM kb_ingest_queue"
                                             " WHERE project='dproj' AND status='pending'",
                                             e, sizeof e);
      assert(qs);
      assert(aimee_pg_step(qs, e, sizeof e) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int64(qs, 0) == 1); /* exactly one queued row */
      assert(aimee_pg_column_int64(qs, 1) == 1); /* force=1 so the drain re-embeds */
      aimee_pg_finalize(qs);

      int q2 = db2_code_index_requeue_drifted();
      assert(q2 == 0); /* dedup: dproj already pending, not re-enqueued */
      printf("  drift requeue enqueues drifted project once, dedups (q1=%d q2=%d) OK\n", q1, q2);

      /* two MORE distinct drifted projects → one call enqueues both (exercises the
       * DISTINCT p.name path with >1 RETURNING row; dproj stays deduped). */
      for (int k = 2; k <= 3; k++)
      {
         char ins[512];
         snprintf(ins, sizeof ins,
                  "INSERT INTO projects (name, root, scanned_at) VALUES ('dproj%d','/x%d','x')", k,
                  k);
         assert(aimee_pg_exec(conn, ins, e, sizeof e) == 0);
         snprintf(ins, sizeof ins,
                  "INSERT INTO files (project_id, path, hash, scanned_at) VALUES"
                  " ((SELECT id FROM projects WHERE name='dproj%d'),'src/s.c','h',"
                  " '2026-06-02T00:00:00Z')",
                  k);
         assert(aimee_pg_exec(conn, ins, e, sizeof e) == 0);
         snprintf(ins, sizeof ins,
                  "INSERT INTO code_embeddings (point_id, project, file_path, node_key,"
                  " updated_at) VALUES (%d,'dproj%d','src/s.c','n','2026-06-01 00:00:00')",
                  200 + k, k);
         assert(aimee_pg_exec(conn, ins, e, sizeof e) == 0);
      }
      int q3 = db2_code_index_requeue_drifted();
      assert(q3 == 2); /* dproj2 + dproj3 new; dproj already pending (deduped) */
      printf("  drift requeue enqueues multiple distinct drifted projects (q3=%d) OK\n", q3);
   }

   db2_test_shim_close();
   printf("code_index_ops: all tests passed\n");
   return 0;
}
