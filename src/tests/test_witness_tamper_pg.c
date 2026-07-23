/* P7-witness-e3 §2 (live-store half): tamper detection against a real Postgres.
 *
 * The two scenarios that need an actual store, because they are about what the
 * store does when an attacker edits it:
 *
 *   1. LOCALLY INCONSISTENT tampering — the attacker edits evidence but leaves the
 *      shard head, or edits the head but leaves the log. Caught unconditionally by
 *      the local cross-check, with no external round trip and no retained copy.
 *
 *   2. COHERENT LOCAL REWRITE — the attacker rewrites the evidence rows AND the
 *      shard head together so every local artifact agrees with every other. Local
 *      verification now PASSES, and that is not a bug: this is precisely the case
 *      the umbrella says is caught only by comparison against externally retained
 *      copies. The test proves both halves — that local checks pass, and that
 *      comparing the pre-tamper emitted stream against the post-tamper one exposes
 *      the rewrite as a fork.
 *
 * An attacker able to rewrite a WORM table has already defeated the append-only
 * triggers, so the test disables them to reach that state. Doing anything less
 * would test the triggers rather than the detection.
 *
 * Reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) if unset. DESTRUCTIVE: run
 * against an isolated database only.
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

static uint8_t g_stream[1 << 20];
static size_t g_len;

static int capture_sink(void *ctx, vault_witness_export_kind_t kind, const uint8_t *frame,
                        size_t len)
{
   (void)ctx;
   (void)kind;
   if (g_len + len > sizeof g_stream)
      return -1;
   memcpy(g_stream + g_len, frame, len);
   g_len += len;
   return 0;
}

static int exec_sql(void *conn, const char *sql)
{
   char err[512];
   return aimee_pg_exec(conn, sql, err, sizeof err);
}

/* Run a statement expected to RAISE, and require the given SQLSTATE. */
static int expect_sqlstate(void *conn, const char *sql, const char *want)
{
   char err[512], state[8] = "";
   int rc = aimee_pg_exec_sqlstate(conn, sql, state, err, sizeof err);
   if (rc == 0)
   {
      fprintf(stderr, "expected SQLSTATE %s but the statement SUCCEEDED: %s\n", want, sql);
      return -1;
   }
   if (strcmp(state, want) != 0)
   {
      fprintf(stderr, "expected SQLSTATE %s, got '%s' (%s)\n", want, state, err);
      return -1;
   }
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

static void disable_worm(void *conn)
{
   assert(exec_sql(conn, "ALTER TABLE kb_vault_witness_log DISABLE TRIGGER USER") == 0);
   assert(exec_sql(conn, "ALTER TABLE kb_vault_witness_shard DISABLE TRIGGER USER") == 0);
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("witness_tamper_pg: SKIP (AIMEE_TEST_PG_URL unset)\n");
      return 0;
   }
   char home[] = "/tmp/aimee-witness-tamper-home-XXXXXX";
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

   for (int i = 0; i < 5; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "t%d", i);
      append_record(conn, sid);
   }

   /* Baseline: the honest store verifies locally, and this is what a consumer
    * retained. Capturing it BEFORE the tamper is the whole point — detection of a
    * coherent rewrite is a comparison, and a comparison needs a prior copy. */
   if (exec_sql(conn, "SELECT org_vault_witness_verify_shard('!kb','!audit')") != 0)
   {
      fprintf(stderr, "baseline shard verification failed on an untampered store\n");
      return 1;
   }
   int64_t cp_seq = -1;
   if (db2_witness_checkpoint_produce(&cp_seq) != DB2_WITNESS_CP_OK)
   {
      fprintf(stderr, "baseline checkpoint production failed\n");
      return 1;
   }
   db2_witness_emit_stats_t s;
   if (db2_witness_emit_run(capture_sink, NULL, 256, &s) != DB2_WITNESS_EMIT_OK)
   {
      fprintf(stderr, "baseline emission failed\n");
      return 1;
   }
   size_t retained_len = g_len; /* the consumer's retained copy ends here */
   printf("witness_tamper_pg: baseline emitted %llu records, checkpoint seq=%lld\n",
          (unsigned long long)s.records_emitted, (long long)cp_seq);

   disable_worm(conn);

   /* -----------------------------------------------------------------------
    * Scenario 1: LOCALLY INCONSISTENT tampering. Edit an evidence row's content
    * without touching its stored record_hash. The local cross-check must catch it
    * with no retained copy involved. */
   assert(exec_sql(conn, "UPDATE kb_vault_witness_log SET source_id='tampered' "
                         "WHERE tenant='!kb' AND provider='!audit' AND shard_seq=3") == 0);
   if (expect_sqlstate(conn, "SELECT org_vault_witness_verify_shard('!kb','!audit')", "P7W01") != 0)
   {
      fprintf(stderr, "SCENARIO 1 FAILED: edited evidence row not caught locally\n");
      return 1;
   }
   /* The checkpoint producer must refuse rather than sign over a divergent shard. */
   int64_t ignored = -1;
   if (db2_witness_checkpoint_produce(&ignored) != DB2_WITNESS_CP_HEAD_MISMATCH)
   {
      fprintf(stderr, "SCENARIO 1 FAILED: producer did not refuse on head_log_mismatch\n");
      return 1;
   }
   assert(exec_sql(conn, "UPDATE kb_vault_witness_log SET source_id='t2' "
                         "WHERE tenant='!kb' AND provider='!audit' AND shard_seq=3") == 0);
   if (exec_sql(conn, "SELECT org_vault_witness_verify_shard('!kb','!audit')") != 0)
   {
      fprintf(stderr, "restoring the row did not restore local verification\n");
      return 1;
   }
   printf("witness_tamper_pg: scenario 1 OK (locally inconsistent tampering caught locally)\n");

   /* -----------------------------------------------------------------------
    * Scenario 2: COHERENT LOCAL REWRITE. Rewrite records 4 and 5 AND recompute
    * every dependent digest and the shard head, so the local store is entirely
    * self-consistent. Local verification must now PASS — and the rewrite must
    * still be exposed by comparing against the retained copy. */
   assert(exec_sql(conn,
                   "DELETE FROM kb_vault_witness_log "
                   "WHERE tenant='!kb' AND provider='!audit' AND shard_seq >= 4") == 0);
   /* Re-append the rewritten tail through the real append function so all digests
    * and the head are recomputed exactly as the production path would. */
   assert(exec_sql(conn, "UPDATE kb_vault_witness_shard SET seq=3, head_hash="
                         "(SELECT record_hash FROM kb_vault_witness_log WHERE tenant='!kb' "
                         "AND provider='!audit' AND shard_seq=3) "
                         "WHERE tenant='!kb' AND provider='!audit'") == 0);
   append_record(conn, "REWRITTEN-4");
   append_record(conn, "REWRITTEN-5");

   if (exec_sql(conn, "SELECT org_vault_witness_verify_shard('!kb','!audit')") != 0)
   {
      fprintf(stderr, "SCENARIO 2 FAILED: coherent rewrite did NOT pass local verification — "
                      "the scenario did not reproduce the case it is meant to test\n");
      return 1;
   }
   printf("witness_tamper_pg: scenario 2 — coherent rewrite passes local verification, "
          "as the threat model predicts\n");

   /* Now emit again from a reset cursor, as a consumer would receive after the
    * attacker's rewrite, and compare the two copies together. */
   assert(exec_sql(conn, "DELETE FROM kb_vault_witness_emit_cursor") == 0);
   db2_witness_emit_stats_t s2;
   if (db2_witness_emit_run(capture_sink, NULL, 256, &s2) != DB2_WITNESS_EMIT_OK)
   {
      fprintf(stderr, "post-tamper emission failed\n");
      return 1;
   }

   uint8_t pub[32], key_id[16];
   assert(vault_witness_signer_identity(pub, key_id) == 0);
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   /* The post-tamper stream ALONE looks clean — the attacker made it consistent. */
   vault_witness_offline_report_t after;
   assert(vault_witness_offline_verify(g_stream + retained_len, g_len - retained_len, &anchor, 1,
                                       &after) == 0);
   if (after.records_conflict != 0)
   {
      fprintf(stderr, "post-tamper stream alone showed a conflict; the rewrite was not coherent "
                      "and scenario 2 did not reproduce\n");
      return 1;
   }

   /* Both copies together expose it: the retained records at positions 4 and 5
    * disagree with the rewritten ones. This is the detection the umbrella claims,
    * and it required the retained copy — nothing else. */
   vault_witness_offline_report_t both;
   assert(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &both) == 0);
   printf("witness_tamper_pg: combined copies -> records=%zu duplicate=%zu conflict=%zu tamper=%d\n",
          both.records, both.records_duplicate, both.records_conflict, both.any_tamper);
   if (both.records_conflict < 2 || !both.any_tamper)
   {
      fprintf(stderr, "SCENARIO 2 FAILED: comparing retained and post-tamper copies did NOT "
                      "expose the coherent rewrite (conflict=%zu tamper=%d)\n",
              both.records_conflict, both.any_tamper);
      return 1;
   }
   printf("witness_tamper_pg: scenario 2 OK (coherent rewrite exposed by comparison with the "
          "retained copy)\n");

   db2_shutdown();
   printf("witness_tamper_pg: PASSED\n");
   return 0;
}
