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
#include "kb/kb_vault_policy.h"
#include "kb/kb_witness_cadence.h"
#include "modules/vault/vault_witness_offline.h"
#include "modules/vault/vault_witness_signer.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Every step of this test either performs the tampering or checks for it, so none
 * of it may be compiled away. assert() would vanish under NDEBUG and leave a
 * security test that passes having done nothing at all — so side effects and
 * assertions go through MUST, which is always evaluated. */
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

/* Returns 0 on success, -1 on failure. Never asserts: appending the evidence IS
 * the test fixture, so it must happen even in a build that strips assertions. */
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
      fprintf(stderr, "append_record(%s): prepare/bind failed: %s\n", sid, err);
      return -1;
   }
   int ok = (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);
   aimee_pg_finalize(st);
   if (!ok)
      fprintf(stderr, "append_record(%s): step failed: %s\n", sid, err);
   return ok ? 0 : -1;
}

static int disable_worm(void *conn)
{
   if (exec_sql(conn, "ALTER TABLE kb_vault_witness_log DISABLE TRIGGER USER") != 0 ||
       exec_sql(conn, "ALTER TABLE kb_vault_witness_shard DISABLE TRIGGER USER") != 0)
   {
      fprintf(stderr, "could not disable WORM triggers; the tamper scenarios cannot run\n");
      return -1;
   }
   return 0;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("witness_tamper_pg: SKIP (AIMEE_TEST_PG_URL unset)\n");
      return 0;
   }
   char home[256];
   snprintf(home, sizeof home, "%s/aimee-witness-tamper-home-XXXXXX", platform_tmpdir());
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
   MUST(conn != NULL, "no database connection");

   for (int i = 0; i < 5; i++)
   {
      char sid[16];
      snprintf(sid, sizeof sid, "t%d", i);
      MUST(append_record(conn, sid) == 0, "baseline append %s failed", sid);
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

   MUST(disable_worm(conn) == 0, "WORM triggers still enabled");

   /* -----------------------------------------------------------------------
    * Scenario 1: LOCALLY INCONSISTENT tampering. Edit an evidence row's content
    * without touching its stored record_hash. The local cross-check must catch it
    * with no retained copy involved. */
   MUST(exec_sql(conn, "UPDATE kb_vault_witness_log SET source_id='tampered' "
                       "WHERE tenant='!kb' AND provider='!audit' AND shard_seq=3") == 0,
        "scenario 1 tamper UPDATE did not run");
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
   MUST(exec_sql(conn, "UPDATE kb_vault_witness_log SET source_id='t2' "
                       "WHERE tenant='!kb' AND provider='!audit' AND shard_seq=3") == 0,
        "scenario 1 restore UPDATE did not run");
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
   MUST(exec_sql(conn, "DELETE FROM kb_vault_witness_log "
                       "WHERE tenant='!kb' AND provider='!audit' AND shard_seq >= 4") == 0,
        "scenario 2 rewrite DELETE did not run");
   /* Re-append the rewritten tail through the real append function so all digests
    * and the head are recomputed exactly as the production path would. */
   MUST(exec_sql(conn, "UPDATE kb_vault_witness_shard SET seq=3, head_hash="
                       "(SELECT record_hash FROM kb_vault_witness_log WHERE tenant='!kb' "
                       "AND provider='!audit' AND shard_seq=3) "
                       "WHERE tenant='!kb' AND provider='!audit'") == 0,
        "scenario 2 head rollback did not run");
   MUST(append_record(conn, "REWRITTEN-4") == 0, "rewritten record 4 not appended");
   MUST(append_record(conn, "REWRITTEN-5") == 0, "rewritten record 5 not appended");

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
   MUST(exec_sql(conn, "DELETE FROM kb_vault_witness_emit_cursor") == 0,
        "emission cursor reset did not run");
   db2_witness_emit_stats_t s2;
   if (db2_witness_emit_run(capture_sink, NULL, 256, &s2) != DB2_WITNESS_EMIT_OK)
   {
      fprintf(stderr, "post-tamper emission failed\n");
      return 1;
   }

   uint8_t pub[32], key_id[16];
   MUST(vault_witness_signer_identity(pub, key_id) == 0, "could not derive the witness identity");
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, 16);
   memcpy(anchor.ed25519_pub, pub, 32);

   /* The post-tamper stream ALONE looks clean — the attacker made it consistent. */
   vault_witness_offline_report_t after;
   MUST(vault_witness_offline_verify(g_stream + retained_len, g_len - retained_len, &anchor, 1,
                                     &after) == 0,
        "offline verification of the post-tamper stream did not run");
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
   MUST(vault_witness_offline_verify(g_stream, g_len, &anchor, 1, &both) == 0,
        "offline verification of the combined stream did not run");
   printf(
       "witness_tamper_pg: combined copies -> records=%zu duplicate=%zu conflict=%zu tamper=%d\n",
       both.records, both.records_duplicate, both.records_conflict, both.any_tamper);
   /* Lock down the checkpoint side too. Without this, a regression that made every
    * checkpoint verify as BAD_SIG or UNKNOWN_KEY would still leave this test green,
    * because the record conflict alone sets any_tamper. The duplicate arises because
    * the cursor was reset and the same checkpoint re-emitted; it must be collapsed,
    * not counted as a fork. */
   if (both.checkpoints_ok < 1 || both.checkpoints_bad_sig != 0 ||
       both.checkpoints_unknown_key != 0 || both.checkpoints_revoked != 0 ||
       both.checkpoints_conflict != 0 || both.continuity == VAULT_WITNESS_CONTINUITY_BROKEN)
   {
      fprintf(stderr,
              "combined-stream checkpoint verification wrong: ok=%zu bad=%zu unknown=%zu "
              "revoked=%zu dup=%zu conflict=%zu continuity=%d\n",
              both.checkpoints_ok, both.checkpoints_bad_sig, both.checkpoints_unknown_key,
              both.checkpoints_revoked, both.checkpoints_duplicate, both.checkpoints_conflict,
              (int)both.continuity);
      return 1;
   }
   if (both.records_conflict < 2 || !both.any_tamper)
   {
      fprintf(stderr,
              "SCENARIO 2 FAILED: comparing retained and post-tamper copies did NOT "
              "expose the coherent rewrite (conflict=%zu tamper=%d)\n",
              both.records_conflict, both.any_tamper);
      return 1;
   }
   printf("witness_tamper_pg: scenario 2 OK (coherent rewrite exposed by comparison with the "
          "retained copy)\n");

   /* -----------------------------------------------------------------------
    * Scenario 3: a retained checkpoint signed by a key this kb cannot derive.
    * A kb holding evidence it cannot verify must refuse to start. Both halves are
    * asserted: coverage is clean beforehand, so the failure below is caused by the
    * foreign key and not by the check being broken in general. */
   {
      uint8_t cur_pub[32], cur_id[16];
      MUST(vault_witness_signer_identity(cur_pub, cur_id) == 0, "identity derivation failed");
      int64_t unknown = -1;
      char sample[64] = "";
      int cov = db2_witness_checkpoint_anchor_coverage(cur_id, sizeof cur_id, &unknown, sample,
                                                       sizeof sample);
      if (cov != 0)
      {
         fprintf(stderr,
                 "SCENARIO 3 FAILED: anchor coverage check itself failed on an honest "
                 "store (returned %d) — the check is broken, not the store\n",
                 cov);
         return 1;
      }
      if (unknown != 0)
      {
         fprintf(stderr, "SCENARIO 3 FAILED: honest store reported %lld unknown signers\n",
                 (long long)unknown);
         return 1;
      }
      /* The checkpoint table is WORM too; the attacker has already defeated it. */
      MUST(exec_sql(conn, "ALTER TABLE kb_vault_witness_checkpoint DISABLE TRIGGER USER") == 0,
           "could not disable checkpoint WORM triggers");
      MUST(exec_sql(conn, "UPDATE kb_vault_witness_checkpoint "
                          "SET signer_key_id = decode(repeat('be',16),'hex')") == 0,
           "signer_key_id substitution did not run");
      if (db2_witness_checkpoint_anchor_coverage(cur_id, sizeof cur_id, &unknown, sample,
                                                 sizeof sample) != 0 ||
          unknown < 1)
      {
         fprintf(stderr, "SCENARIO 3 FAILED: foreign signer_key_id not reported (unknown=%lld)\n",
                 (long long)unknown);
         return 1;
      }
      /* And the boot gate itself must refuse, with an operator-actionable reason. */
      char err[512] = "";
      if (kb_vault_live_keys_allowed())
      {
         if (kb_witness_boot_check(err, sizeof err) == 0)
         {
            fprintf(stderr, "SCENARIO 3 FAILED: boot check passed with unverifiable evidence\n");
            return 1;
         }
         printf("witness_tamper_pg: boot refused: %s\n", err);
      }
      else
      {
         /* A dev kb with no live keys witnesses nothing that gates a real key, so
          * the boot gate deliberately does not apply. Say so rather than silently
          * reporting a pass this environment never exercised. */
         printf("witness_tamper_pg: boot gate not exercised (no live keys in this env); "
                "coverage check itself verified: unknown=%lld sample=%s\n",
                (long long)unknown, sample);
      }
      printf("witness_tamper_pg: scenario 3 OK (unverifiable checkpoint detected)\n");
   }

   /* -----------------------------------------------------------------------
    * Scenario 4: continuous verification of the retained checkpoint run. The
    * per-tick shard cross-check cannot see a forged SIGNATURE over an otherwise
    * consistent shard, so this is a distinct detection path and is tested as one.
    * The foreign signer_key_id planted above is restored first so this measures the
    * signature check, not the leftover from scenario 3. */
   {
      db2_witness_verify_report_t vr;
      uint8_t cur_pub[32], cur_id[16];
      MUST(vault_witness_signer_identity(cur_pub, cur_id) == 0, "identity derivation failed");

      /* Unknown key is detected. */
      if (db2_witness_checkpoint_verify_run(256, &vr) != 0)
      {
         fprintf(stderr, "SCENARIO 4 FAILED: verify run could not execute\n");
         return 1;
      }
      if (vr.unknown_key < 1)
      {
         fprintf(stderr,
                 "SCENARIO 4 FAILED: foreign signer key not reported by verify run "
                 "(checked=%lld unknown=%lld)\n",
                 (long long)vr.checked, (long long)vr.unknown_key);
         return 1;
      }

      /* Now isolate the SIGNATURE check from the key-identity check. Restoring the
       * real signer_key_id alone is not enough to keep this meaningful: that field
       * is part of the signed body, so putting it back reproduces the exact bytes
       * that were signed and the signature legitimately verifies again. To test the
       * signature path the signature itself must be corrupted. */
      {
         char sql[256];
         char hex[33];
         for (int i = 0; i < 16; i++)
            snprintf(hex + i * 2, 3, "%02x", cur_id[i]);
         snprintf(sql, sizeof sql,
                  "UPDATE kb_vault_witness_checkpoint SET signer_key_id = decode('%s','hex')", hex);
         MUST(exec_sql(conn, sql) == 0, "signer_key_id restore did not run");
         /* Sanity: with the body restored the run must be clean again, otherwise the
          * bad-signature result below would not be attributable to the corruption. */
         if (db2_witness_checkpoint_verify_run(256, &vr) != 0 || vr.bad_signature != 0 ||
             vr.unknown_key != 0)
         {
            fprintf(stderr,
                    "SCENARIO 4 FAILED: restoring the signed body did not restore a clean "
                    "verdict (bad=%lld unknown=%lld)\n",
                    (long long)vr.bad_signature, (long long)vr.unknown_key);
            return 1;
         }
         MUST(exec_sql(conn, "UPDATE kb_vault_witness_checkpoint "
                             "SET signature = decode(repeat('7f',64),'hex')") == 0,
              "signature corruption did not run");
      }
      if (db2_witness_checkpoint_verify_run(256, &vr) != 0)
      {
         fprintf(stderr, "SCENARIO 4 FAILED: verify run could not execute after restore\n");
         return 1;
      }
      if (vr.bad_signature < 1)
      {
         fprintf(stderr,
                 "SCENARIO 4 FAILED: checkpoint whose signer_key_id was swapped still "
                 "verified (checked=%lld bad=%lld)\n",
                 (long long)vr.checked, (long long)vr.bad_signature);
         return 1;
      }
      printf("witness_tamper_pg: scenario 4 OK (continuous verification catches unknown key and "
             "bad signature: checked=%lld bad=%lld)\n",
             (long long)vr.checked, (long long)vr.bad_signature);
   }

   db2_shutdown();
   printf("witness_tamper_pg: PASSED\n");
   return 0;
}
