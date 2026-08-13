/* P7-witness-e3 §1 (process-level half): emission/checkpoint recovery across a
 * simulated restart, on a REAL Postgres.
 *
 * The §1 atomicity gate (scripts/p7_witness_atomicity_pg_test.sql) covers every
 * kill-matrix boundary that lies INSIDE a source transaction: there a process kill
 * and a transaction abort are the same event to Postgres, so aborting proves it.
 *
 * What that does NOT cover is the boundaries that are genuinely about process
 * lifecycle — durable state committed, the process gone, a fresh process re-reading
 * only what survived:
 *
 *   Boundary 9  — a checkpoint COMMITs, the process dies before it is emitted.
 *   Boundary 10 — leaf-snapshot / checkpoint emission dies after the checkpoint
 *                 frame is sunk but before its snapshot.
 *   Boundary 11 — record emission dies MID-BATCH.
 *
 * A restart is simulated by db2_shutdown() + db2_init(): the connection pool is torn
 * down and a fresh one re-reads the durable tables and the emission cursor. Nothing
 * in-process survives, which is exactly the post-kill starting state the plan
 * specifies ("restart using only PostgreSQL ... and the artifact directory").
 *
 * Required outcomes asserted here:
 *   - a committed-but-unemitted checkpoint is emitted after restart (never lost);
 *   - emission killed mid-batch resumes with NO record skipped and NO sequence gap;
 *   - the post-restart combined stream verifies clean offline — re-emitted frames
 *     are benign duplicates, never a fork;
 *   - a restart after a full drain is a no-op (no re-emission storm).
 *
 * Reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) if unset. DESTRUCTIVE: run
 * against an isolated database only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db2_witness_checkpoint.h"
#include "db2/db2_witness_emit.h"
#include "db2/db_postgres.h"
#include "modules/vault/vault_witness_offline.h"
#include "modules/vault/vault_witness_signer.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define MUST(cond, ...)                                                                            \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         fprintf(stderr, "FAILED (%s:%d): ", __FILE__, __LINE__);                                  \
         fprintf(stderr, __VA_ARGS__);                                                             \
         fprintf(stderr, "\n");                                                                    \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

/* Captured emission stream, plus a sink that can be told to fail after N accepts to
 * model a process that dies mid-batch. */
static uint8_t g_stream[1 << 22];
static size_t g_len;
static int g_sink_budget = -1; /* -1 = unlimited; otherwise accept this many then fail */

static int capture_sink(void *ctx, vault_witness_export_kind_t kind, const uint8_t *frame,
                        size_t len)
{
   (void)ctx;
   (void)kind;
   if (g_sink_budget == 0)
      return -1; /* "process died": this frame is not durably off-host */
   if (g_sink_budget > 0)
      g_sink_budget--;
   if (g_len + len > sizeof g_stream)
      return -1;
   memcpy(g_stream + g_len, frame, len);
   g_len += len;
   return 0;
}

static char g_url[512];

/* Tear down and re-open the db2 layer: the process-restart stand-in. Returns the
 * fresh connection or NULL. */
static void *simulate_restart(void)
{
   db2_shutdown();
   if (db2_init(g_url) != 0)
      return NULL;
   return db2_conn();
}

static int append_record(void *conn, const char *sid)
{
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT * FROM org_vault_witness_append(0::smallint,?1,'!kb','!audit','','p','','g',"
       "'2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),true,decode(repeat('00',32),'hex'))",
       err, sizeof err);
   if (!st || aimee_pg_bind_text(st, "?1", sid) != 0)
   {
      if (st)
         aimee_pg_finalize(st);
      return -1;
   }
   int ok = (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("witness_recovery_pg: SKIP (AIMEE_TEST_PG_URL unset)\n");
      return 0;
   }
   snprintf(g_url, sizeof g_url, "%s", url);
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-witness-recovery-home-XXXXXX", platform_tmpdir());
   MUST(mkdtemp(home) != NULL, "mkdtemp failed");
   setenv("AIMEE_HOME", home, 1);
   MUST(db2_init(url) == 0, "db2_init failed for %s", url);
   void *conn = db2_conn();
   MUST(conn != NULL, "no connection");

   /* Derive the anchor once; it is stable across restarts (KEK-derived). */
   uint8_t pub[32], key_id[16];
   MUST(vault_witness_signer_identity(pub, key_id) == 0, "identity derivation failed");
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   /* -----------------------------------------------------------------------
    * Boundary 9: a checkpoint COMMITs, the process dies before emission.
    * Append, checkpoint, then restart WITHOUT emitting. The committed checkpoint
    * and all its records must be emitted by the post-restart run. */
   for (int i = 1; i <= 5; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "b9-%d", i);
      MUST(append_record(conn, sid) == 0, "append %s failed", sid);
   }
   int64_t cp1 = -1;
   MUST(db2_witness_checkpoint_produce(&cp1) == DB2_WITNESS_CP_OK, "checkpoint produce failed");

   conn = simulate_restart();
   MUST(conn != NULL, "restart after checkpoint-commit failed");

   db2_witness_emit_stats_t s9;
   MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &s9) == DB2_WITNESS_EMIT_OK,
        "post-restart emit failed");
   MUST(s9.records_emitted == 5, "committed-but-unemitted: expected 5 records, got %llu",
        (unsigned long long)s9.records_emitted);
   MUST(s9.checkpoints_emitted == 1,
        "committed-but-unemitted checkpoint was LOST after restart "
        "(emitted %llu)",
        (unsigned long long)s9.checkpoints_emitted);
   printf("witness_recovery_pg: boundary 9 OK (checkpoint committed pre-kill is emitted after "
          "restart)\n");

   /* -----------------------------------------------------------------------
    * Boundaries 10-11: emission dies MID-BATCH. Append more records, then emit with
    * a sink that accepts only a few frames before "dying". The cursor advances only
    * as far as it durably got; a restart must resume with no gap and no loss. */
   for (int i = 6; i <= 15; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "b11-%d", i);
      MUST(append_record(conn, sid) == 0, "append %s failed", sid);
   }
   /* First, the strongest cursor guarantee: a kill BEFORE any frame is durably sunk
    * must not advance the cursor at all. Emit with a sink that fails on frame 0, then
    * confirm a restart re-reads from the same position and loses nothing — proving
    * the cursor never runs ahead of durable emission. */
   {
      g_sink_budget = 0; /* the process dies before the first frame lands */
      db2_witness_emit_stats_t sz;
      db2_witness_emit_result_t rz = db2_witness_emit_run(capture_sink, NULL, 8192, &sz);
      MUST(rz == DB2_WITNESS_EMIT_SINK_FAILED, "expected an immediate sink failure, got %d",
           (int)rz);
      MUST(sz.records_emitted == 0, "the cursor advanced with nothing durably sunk (%llu records)",
           (unsigned long long)sz.records_emitted);
      g_sink_budget = -1;
      conn = simulate_restart();
      MUST(conn != NULL, "restart after zero-sink kill failed");
      db2_witness_emit_stats_t szr;
      MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &szr) == DB2_WITNESS_EMIT_OK,
           "resume after zero-sink kill failed");
      MUST(szr.records_emitted == 10,
           "zero-sink kill lost records: resume emitted %llu, expected all 10",
           (unsigned long long)szr.records_emitted);
      printf("witness_recovery_pg: zero-sink kill OK (cursor did not advance; all 10 records "
             "recovered)\n");
   }

   /* Re-append the same 10 records' worth for the mid-batch case below (the run
    * above already drained 6..15). Use a fresh range so the chain stays contiguous. */
   for (int i = 16; i <= 25; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "b11-%d", i);
      MUST(append_record(conn, sid) == 0, "append %s failed", sid);
   }
   size_t before_partial = g_len;
   g_sink_budget = 4; /* accept 4 frames, then the "process dies" */
   db2_witness_emit_stats_t s10;
   db2_witness_emit_result_t r10 = db2_witness_emit_run(capture_sink, NULL, 8192, &s10);
   MUST(r10 == DB2_WITNESS_EMIT_SINK_FAILED,
        "expected the mid-batch sink failure to surface, got %d", (int)r10);
   MUST(g_len > before_partial, "no frames were captured before the simulated kill");
   printf("witness_recovery_pg: mid-batch kill after %llu records (%zu bytes captured)\n",
          (unsigned long long)s10.records_emitted, g_len - before_partial);

   g_sink_budget = -1; /* the replacement process has a healthy sink */
   conn = simulate_restart();
   MUST(conn != NULL, "restart after mid-batch kill failed");

   db2_witness_emit_stats_t s11;
   MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &s11) == DB2_WITNESS_EMIT_OK,
        "post-kill resume emit failed");
   printf("witness_recovery_pg: resumed, emitted %llu more records\n",
          (unsigned long long)s11.records_emitted);

   /* The whole retained stream — the partial pre-kill bytes plus everything after —
    * must verify clean: re-emitted frames are benign duplicates, never a fork, and
    * there must be no sequence gap in any shard. This is the property that proves a
    * mid-batch kill loses nothing and corrupts nothing. */
   vault_witness_offline_report_t rep;
   MUST(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0,
        "offline verification did not run");
   printf("witness_recovery_pg: combined stream records=%zu dup=%zu conflict=%zu shards_ok=%zu "
          "shards_broken=%zu cp_ok=%zu cp_dup=%zu tamper=%d malformed=%d\n",
          rep.records, rep.records_duplicate, rep.records_conflict, rep.shards_ok,
          rep.shards_broken, rep.checkpoints_ok, rep.checkpoints_duplicate, rep.any_tamper,
          rep.malformed);
   MUST(rep.any_tamper == 0, "the post-restart retained stream was flagged as tampered");
   MUST(rep.malformed == 0, "the post-restart retained stream had malformed frames");
   MUST(rep.records_conflict == 0, "a re-emitted record was mistaken for a fork");
   MUST(rep.shards_broken == 0, "a shard chain broke across the restart (a gap or bad link)");

   /* Every appended record (1..15 across the two phases) must be present at least
    * once. The offline verifier collapses duplicates, so shards_ok covering the one
    * shard with a contiguous chain is the proof there is no hole. */
   MUST(rep.shards_ok >= 1, "the audit shard did not verify as a contiguous chain");

   /* -----------------------------------------------------------------------
    * Boundary 10: the checkpoint frame is sunk, the process dies before its leaf
    * snapshot. The emitter does not advance the checkpoint cursor past a checkpoint
    * whose snapshot did not go out, so a restart RE-EMITS that checkpoint. The
    * re-emitted checkpoint must be collapsed as a benign duplicate (this is exactly
    * the case that, before the dedup fix, was mis-reported as a fork) and its
    * snapshot must now be delivered. */
   for (int i = 16; i <= 18; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "b10-%d", i);
      MUST(append_record(conn, sid) == 0, "append %s failed", sid);
   }
   /* Drain the new records first so the next emit run's first frames are the
    * checkpoint and its snapshot, letting us kill precisely between them. */
   db2_witness_emit_stats_t sdrain;
   MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &sdrain) == DB2_WITNESS_EMIT_OK,
        "pre-checkpoint drain failed");
   int64_t cp2 = -1;
   MUST(db2_witness_checkpoint_produce(&cp2) == DB2_WITNESS_CP_OK, "second checkpoint failed");
   g_sink_budget = 1; /* accept the checkpoint frame, die before its snapshot */
   db2_witness_emit_stats_t s10b;
   db2_witness_emit_result_t r10b = db2_witness_emit_run(capture_sink, NULL, 8192, &s10b);
   MUST(r10b == DB2_WITNESS_EMIT_SINK_FAILED, "expected snapshot-sink failure, got %d", (int)r10b);
   MUST(s10b.checkpoints_emitted == 1 && s10b.snapshots_emitted == 0,
        "expected checkpoint sunk but snapshot not (cp=%llu snap=%llu)",
        (unsigned long long)s10b.checkpoints_emitted, (unsigned long long)s10b.snapshots_emitted);

   g_sink_budget = -1;
   conn = simulate_restart();
   MUST(conn != NULL, "restart after snapshot-kill failed");
   db2_witness_emit_stats_t s10c;
   MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &s10c) == DB2_WITNESS_EMIT_OK,
        "post-snapshot-kill resume failed");
   MUST(s10c.checkpoints_emitted == 1 && s10c.snapshots_emitted == 1,
        "resume did not re-emit the checkpoint and deliver its snapshot (cp=%llu snap=%llu)",
        (unsigned long long)s10c.checkpoints_emitted, (unsigned long long)s10c.snapshots_emitted);

   vault_witness_offline_report_t rep2;
   MUST(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep2) == 0,
        "offline verification did not run after boundary 10");
   /* The collapse-vs-fork dedup contract this relies on is itself unit-tested with
    * negative controls in test_vault_witness_offline.c (test_duplicate_checkpoint_
    * tolerated / test_conflicting_checkpoint_is_fork): byte-identical same-seq ->
    * duplicate, different same-seq -> conflict. Here we assert the recovered stream
    * exercises the duplicate side, not the conflict side. */
   MUST(rep2.checkpoints_conflict == 0,
        "the re-emitted checkpoint was mistaken for a fork (conflict=%zu)",
        rep2.checkpoints_conflict);
   MUST(rep2.checkpoints_duplicate >= 1,
        "the re-emitted checkpoint was not seen as a benign duplicate (dup=%zu)",
        rep2.checkpoints_duplicate);
   MUST(rep2.any_tamper == 0, "boundary-10 combined stream flagged as tampered");
   MUST(rep2.snapshots_ok >= 1 && rep2.snapshots_root_mismatch == 0 && rep2.snapshots_bad == 0,
        "the recovered snapshot did not verify (ok=%zu bad=%zu root_mismatch=%zu)",
        rep2.snapshots_ok, rep2.snapshots_bad, rep2.snapshots_root_mismatch);
   printf("witness_recovery_pg: boundary 10 OK (checkpoint re-emitted as benign duplicate, "
          "snapshot recovered)\n");

   /* -----------------------------------------------------------------------
    * Restart after a full drain is a no-op: no re-emission storm. */
   conn = simulate_restart();
   MUST(conn != NULL, "restart after full drain failed");
   db2_witness_emit_stats_t s12;
   MUST(db2_witness_emit_run(capture_sink, NULL, 8192, &s12) == DB2_WITNESS_EMIT_OK,
        "post-drain emit failed");
   MUST(s12.records_emitted == 0 && s12.checkpoints_emitted == 0,
        "restart after a full drain re-emitted (records=%llu checkpoints=%llu) — a cursor that "
        "did not survive the restart",
        (unsigned long long)s12.records_emitted, (unsigned long long)s12.checkpoints_emitted);
   printf("witness_recovery_pg: restart after full drain is a no-op (cursor survived)\n");

   db2_shutdown();
   printf("witness_recovery_pg: PASSED\n");
   return 0;
}
