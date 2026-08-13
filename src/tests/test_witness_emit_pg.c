/* P7-witness-e2 emission integration test (REAL PG ONLY).
 *
 * The end-to-end claim this proves: bytes that leave aimee-kb on the log path are
 * sufficient, on their own, to verify the evidence chain on a machine that has
 * only the trust anchor. Nothing here consults the database during verification.
 *
 * Flow: append witness records -> produce a signed checkpoint -> run the emitter
 * with a sink that captures the framed bytes -> hand the captured stream and the
 * vault-derived anchor to vault_witness_offline_verify (the same core the
 * aimee-witness-verify tool runs) and require a clean verdict.
 *
 * Also asserts the two properties the emitter is responsible for: digest parity
 * between stored rows and re-encoded evidence (implicitly — a parity failure
 * aborts the run), and cursor monotonicity (a second run emits nothing new).
 *
 * Reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) if unset.
 */
#include <assert.h>
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

/* Captured emission stream. */
static uint8_t g_stream[1 << 22];
static size_t g_len;
static size_t g_frames;

static int capture_sink(void *ctx, vault_witness_export_kind_t kind, const uint8_t *frame,
                        size_t len)
{
   (void)ctx;
   (void)kind;
   if (g_len + len > sizeof g_stream)
      return -1;
   memcpy(g_stream + g_len, frame, len);
   g_len += len;
   g_frames++;
   return 0;
}

static void append_record(void *conn, const char *sid)
{
   char err[256];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT * FROM org_vault_witness_append(0::smallint,?1,'!kb','!audit','','p','','g',"
       "'2026-07-23T00:00:00Z',decode(repeat('a1',32),'hex'),true,decode(repeat('00',32),'hex'))",
       err, sizeof err);
   assert(st && aimee_pg_bind_text(st, "?1", sid) == 0);
   assert(aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);
   aimee_pg_finalize(st);
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("witness_emit_pg: SKIP (AIMEE_TEST_PG_URL unset)\n");
      return 0;
   }
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-witness-emit-home-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "mkdtemp failed\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "db2_init failed for %s\n", url);
      return 1;
   }
   void *conn = db2_conn();
   assert(conn);

   append_record(conn, "e1");
   append_record(conn, "e2");
   append_record(conn, "e3");

   int64_t seq = -1;
   db2_witness_checkpoint_result_t cr = db2_witness_checkpoint_produce(&seq);
   if (cr != DB2_WITNESS_CP_OK)
   {
      fprintf(stderr, "produce returned %d\n", (int)cr);
      return 1;
   }

   db2_witness_emit_stats_t s;
   db2_witness_emit_result_t er = db2_witness_emit_run(capture_sink, NULL, 256, &s);
   if (er != DB2_WITNESS_EMIT_OK)
   {
      fprintf(stderr, "emit run returned %d\n", (int)er);
      return 1;
   }
   printf("witness_emit_pg: emitted records=%llu checkpoints=%llu snapshots=%llu (%zu frames, "
          "%zu bytes)\n",
          (unsigned long long)s.records_emitted, (unsigned long long)s.checkpoints_emitted,
          (unsigned long long)s.snapshots_emitted, g_frames, g_len);
   if (s.records_emitted < 3 || s.checkpoints_emitted < 1 || s.snapshots_emitted < 1)
   {
      fprintf(stderr, "emitted too little: r=%llu c=%llu s=%llu\n",
              (unsigned long long)s.records_emitted, (unsigned long long)s.checkpoints_emitted,
              (unsigned long long)s.snapshots_emitted);
      return 1;
   }

   /* Cursor monotonicity: with nothing new appended, a second run emits nothing.
    * If this regressed, every tick would re-emit the whole history. */
   size_t before = g_frames;
   db2_witness_emit_stats_t s2;
   assert(db2_witness_emit_run(capture_sink, NULL, 256, &s2) == DB2_WITNESS_EMIT_OK);
   if (g_frames != before || s2.records_emitted != 0 || s2.checkpoints_emitted != 0)
   {
      fprintf(stderr, "second emit run was not a no-op: frames %zu -> %zu\n", before, g_frames);
      return 1;
   }

   /* A newly appended record is picked up by the next run, and only that record. */
   append_record(conn, "e4");
   db2_witness_emit_stats_t s3;
   assert(db2_witness_emit_run(capture_sink, NULL, 256, &s3) == DB2_WITNESS_EMIT_OK);
   if (s3.records_emitted != 1)
   {
      fprintf(stderr, "incremental emit sent %llu records, expected 1\n",
              (unsigned long long)s3.records_emitted);
      return 1;
   }

   /* Drain across batch boundaries. The reader works in bounded batches, so a
    * backlog larger than one batch must still clear in a single run — otherwise a
    * burst would trickle out one batch per tick and the off-host copy would lag by
    * however long the burst was. 600 crosses the 256-row batch size twice. */
   {
      char sid[24];
      for (int i = 0; i < 600; i++)
      {
         snprintf(sid, sizeof sid, "burst%d", i);
         append_record(conn, sid);
      }
      db2_witness_emit_stats_t sb;
      assert(db2_witness_emit_run(capture_sink, NULL, 8192, &sb) == DB2_WITNESS_EMIT_OK);
      if (sb.records_emitted != 600)
      {
         fprintf(stderr, "burst drain emitted %llu records, expected 600 in one run\n",
                 (unsigned long long)sb.records_emitted);
         return 1;
      }
      if (sb.backlog_records != 0)
      {
         fprintf(stderr, "burst drain left backlog %llu, expected 0\n",
                 (unsigned long long)sb.backlog_records);
         return 1;
      }
      /* And the budget is a real bound, not decoration: a small budget must stop
       * short and leave the rest for the next run rather than draining anyway. */
      for (int i = 0; i < 600; i++)
      {
         snprintf(sid, sizeof sid, "burst2-%d", i);
         append_record(conn, sid);
      }
      db2_witness_emit_stats_t sc;
      assert(db2_witness_emit_run(capture_sink, NULL, 300, &sc) == DB2_WITNESS_EMIT_OK);
      if (sc.records_emitted >= 600 || sc.records_emitted == 0)
      {
         fprintf(stderr, "budget of 300 emitted %llu records; expected a partial drain\n",
                 (unsigned long long)sc.records_emitted);
         return 1;
      }
      printf("witness_emit_pg: burst drained 600 in one run; budget 300 stopped at %llu\n",
             (unsigned long long)sc.records_emitted);
      /* Finish the drain so the offline verification below sees a complete chain. */
      db2_witness_emit_stats_t sd;
      assert(db2_witness_emit_run(capture_sink, NULL, 8192, &sd) == DB2_WITNESS_EMIT_OK);
   }

   /* Verify the captured bytes offline: anchor only, no database. */
   uint8_t pub[32], key_id[16];
   assert(vault_witness_signer_identity(pub, key_id) == 0);
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   vault_witness_offline_report_t rep;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &rep) == 0);
   printf("witness_emit_pg: offline frames=%zu records=%zu checkpoints=%zu snapshots=%zu/ok=%zu "
          "shards_ok=%zu cp_ok=%zu tamper=%d malformed=%d\n",
          rep.frames, rep.records, rep.checkpoints, rep.snapshots, rep.snapshots_ok, rep.shards_ok,
          rep.checkpoints_ok, rep.any_tamper, rep.malformed);
   if (rep.any_tamper || rep.malformed)
   {
      fprintf(stderr, "offline verification of the EMITTED stream failed\n");
      return 1;
   }
   if (rep.shards_ok < 1 || rep.checkpoints_ok < 1)
   {
      fprintf(stderr, "offline verification saw no verified shard/checkpoint\n");
      return 1;
   }
   /* The emitted leaf snapshot must verify against the digest its own checkpoint
    * signature commits to — proving the emitted snapshot really is the leaf set
    * the producer signed, not just well-formed bytes. */
   /* snapshots_ok now requires BOTH bindings: the bytes are the ones the signature
    * committed to, AND their leaves rebuild the signed root. This is what proves the
    * producer's stored snapshot format and the verifier's parser actually agree —
    * a divergence there would make every emitted snapshot useless while still
    * passing a digest-only check. */
   if (rep.snapshots_ok < 1 || rep.snapshots_bad != 0 || rep.snapshots_root_mismatch != 0)
   {
      fprintf(stderr,
              "emitted leaf snapshot did not verify: ok=%zu bad_digest=%zu root_mismatch=%zu "
              "unmatched=%zu\n",
              rep.snapshots_ok, rep.snapshots_bad, rep.snapshots_root_mismatch,
              rep.snapshots_unmatched);
      return 1;
   }

   /* Tamper with one emitted record byte and require the offline verifier to catch
    * it. Without this the clean pass above proves only that nothing went wrong. */
   g_stream[VAULT_WITNESS_EXPORT_HEADER_LEN + 60] ^= 0xFF;
   vault_witness_offline_report_t bad;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &bad) == 0);
   if (!bad.any_tamper)
   {
      fprintf(stderr, "tampered emitted stream verified clean — detection is broken\n");
      return 1;
   }

   db2_shutdown();
   printf("witness_emit_pg: PASSED (emitted bytes verify offline; tampering detected)\n");
   return 0;
}
