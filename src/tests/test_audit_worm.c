/* test_audit_worm.c: S0 WORM audit store — chain integrity, gap-free seq,
 * WORM triggers, cross-store determinism, and crypto tamper detection. */
#include <assert.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audit_worm.h"

static char g_dir[256];

static void mk_tmpdir(void)
{
   snprintf(g_dir, sizeof g_dir, "/tmp/worm_test_XXXXXX");
   assert(mkdtemp(g_dir) != NULL);
}

static void db_path(char *out, size_t n)
{
   snprintf(out, n, "%s/w.db", g_dir);
}

/* Append + chain verifies, count is exact, seq is gap-free. */
static void test_append_and_chain(void)
{
   char path[300];
   db_path(path, sizeof path);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u1", "tool.read", "v1-abc", "allow", "{\"k\":1}") == 0);
   assert(audit_worm_append("delegate", "mimo", "tool.write", "v1-def", "block", "{\"k\":2}") == 0);
   assert(audit_worm_append("primary", "u1", "agent.set", "mimo", "allow", "{}") == 0);
   assert(audit_worm_count() == 3);
   char err[128];
   assert(audit_worm_verify_chain(err, sizeof err) == 0);
   audit_worm_close();
   printf("  test_append_and_chain: ok\n");
}

/* The BEFORE UPDATE/DELETE triggers reject mutation of a committed row. */
static void test_worm_triggers_block_mutation(void)
{
   char path[300];
   db_path(path, sizeof path);
   sqlite3 *raw = NULL;
   assert(sqlite3_open(path, &raw) == SQLITE_OK);
   char *emsg = NULL;
   int urc =
       sqlite3_exec(raw, "UPDATE audit_event SET verdict='allow' WHERE seq=2", NULL, NULL, &emsg);
   assert(urc != SQLITE_OK); /* RAISE(ABORT) */
   sqlite3_free(emsg);
   emsg = NULL;
   int drc = sqlite3_exec(raw, "DELETE FROM audit_event WHERE seq=1", NULL, NULL, &emsg);
   assert(drc != SQLITE_OK);
   sqlite3_free(emsg);
   sqlite3_close(raw);
   printf("  test_worm_triggers_block_mutation: ok\n");
}

/* Same inputs, two independent stores -> identical row_hash for seq=1. This is
 * the reproducibility the cross-engine (SQLite/Postgres) vectors rest on. */
static void test_cross_store_determinism(void)
{
   char pa[300], pb[300];
   snprintf(pa, sizeof pa, "%s/a.db", g_dir);
   snprintf(pb, sizeof pb, "%s/b.db", g_dir);

   char ha[65] = {0}, hb[65] = {0};
   for (int i = 0; i < 2; i++)
   {
      const char *p = i == 0 ? pa : pb;
      assert(audit_worm_init_at(p) == 0);
      assert(audit_worm_append("primary", "u1", "tool.read", "v1-fixed", "allow", "{\"x\":1}") ==
             0);
      audit_worm_close();
      sqlite3 *raw = NULL;
      assert(sqlite3_open(p, &raw) == SQLITE_OK);
      sqlite3_stmt *q = NULL;
      assert(sqlite3_prepare_v2(raw, "SELECT row_hash, prev_hash FROM audit_event WHERE seq=1", -1,
                                &q, NULL) == SQLITE_OK);
      assert(sqlite3_step(q) == SQLITE_ROW);
      snprintf(i == 0 ? ha : hb, 65, "%s", (const char *)sqlite3_column_text(q, 0));
      /* genesis prev is 32 zero bytes (hex) */
      assert(strcmp((const char *)sqlite3_column_text(q, 1), AUDIT_WORM_GENESIS_PREV) == 0);
      sqlite3_finalize(q);
      sqlite3_close(raw);
   }
   assert(ha[0] && strcmp(ha, hb) == 0);
   printf("  test_cross_store_determinism: ok\n");
}

/* Tampering is detected by the chain EVEN when the WORM triggers are dropped —
 * i.e. the guarantee is the crypto, not the DB triggers. */
static void test_tamper_detected_past_triggers(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/t.db", g_dir);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u1", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(audit_worm_append("primary", "u1", "tool.read", "v1-2", "allow", "{}") == 0);
   assert(audit_worm_verify_chain(NULL, 0) == 0);
   audit_worm_close();

   /* Bypass WORM: drop the triggers, then rewrite a committed row's subject. */
   sqlite3 *raw = NULL;
   assert(sqlite3_open(path, &raw) == SQLITE_OK);
   assert(sqlite3_exec(raw,
                       "DROP TRIGGER audit_event_no_update;"
                       "DROP TRIGGER audit_event_no_delete;"
                       "UPDATE audit_event SET subject='v1-EVIL' WHERE seq=1",
                       NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(raw);

   assert(audit_worm_init_at(path) == 0);
   char err[128];
   assert(audit_worm_verify_chain(err, sizeof err) == -1);
   assert(strstr(err, "seq 1") != NULL);
   audit_worm_close();
   printf("  test_tamper_detected_past_triggers: ok (%s)\n", err);
}

/* Checkpoints move verify from AMBER (uncheckpointed tail) to GREEN, and back to
 * AMBER once new rows land after the newest checkpoint. */
static void test_checkpoint_and_verify_status(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/ck.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1); /* chain key -> g_dir/.audit-chain-key */
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-2", "allow", "{}") == 0);

   long head = 0, ck = 0;
   char err[160];
   assert(audit_worm_verify(err, sizeof err, &head, &ck) == AUDIT_WORM_VERIFY_AMBER);
   assert(head == 2 && ck == 0);

   assert(audit_worm_checkpoint() == 0); /* row seq=3 attests head seq=2 */
   assert(audit_worm_verify(err, sizeof err, &head, &ck) == AUDIT_WORM_VERIFY_GREEN);
   assert(head == 3 && ck == 3);

   assert(audit_worm_append("primary", "u", "tool.read", "v1-3", "allow", "{}") == 0);
   assert(audit_worm_verify(err, sizeof err, NULL, NULL) == AUDIT_WORM_VERIFY_AMBER);
   audit_worm_close();
   printf("  test_checkpoint_and_verify_status: ok\n");
}

/* A checkpoint is bound to the chain key: verifying against a different key (an
 * attacker who can rewrite the file but lacks the key) is detected as RED. */
static void test_checkpoint_bound_to_chain_key(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/ckkey.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(audit_worm_checkpoint() == 0);
   char err[160];
   assert(audit_worm_verify(err, sizeof err, NULL, NULL) == AUDIT_WORM_VERIFY_GREEN);
   audit_worm_close();

   /* Swap the chain key for a different one; the checkpoint no longer verifies. */
   char keypath[400];
   snprintf(keypath, sizeof keypath, "%s/.audit-chain-key", g_dir);
   int fd = open(keypath, O_WRONLY | O_TRUNC);
   assert(fd >= 0);
   unsigned char bogus[32];
   memset(bogus, 0xAB, sizeof bogus);
   assert(write(fd, bogus, sizeof bogus) == (ssize_t)sizeof bogus);
   close(fd);

   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_verify(err, sizeof err, NULL, NULL) == AUDIT_WORM_VERIFY_RED);
   assert(strstr(err, "checkpoint") != NULL);
   audit_worm_close();
   printf("  test_checkpoint_bound_to_chain_key: ok (%s)\n", err);
}

/* Sealing exports an immutable, independently-verifiable snapshot. */
static void test_seal_snapshot_verifies(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/seal.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-2", "allow", "{}") == 0);

   char sealed[512] = "";
   int imm = -1;
   assert(audit_worm_seal(sealed, sizeof sealed, &imm) == 0);
   assert(sealed[0]);
   char err[160];
   assert(audit_worm_verify_file(sealed, err, sizeof err) == 0); /* snapshot verifies green */
   audit_worm_close();
   printf("  test_seal_snapshot_verifies: ok (immutable=%d)\n", imm);
}

/* Tampering a sealed snapshot (when the OS immutable flag isn't enforced) is still
 * caught by the crypto chain — the guarantee, per R2-7, is the crypto not the flag. */
static void test_sealed_snapshot_tamper_detected(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/seal2.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   char sealed[512] = "";
   int imm = -1;
   assert(audit_worm_seal(sealed, sizeof sealed, &imm) == 0);
   audit_worm_close();

   if (imm)
   {
      printf("  test_sealed_snapshot_tamper_detected: skipped (snapshot is OS-immutable)\n");
      return;
   }
   sqlite3 *raw = NULL;
   assert(sqlite3_open(sealed, &raw) == SQLITE_OK);
   assert(sqlite3_exec(raw,
                       "DROP TRIGGER audit_event_no_update;"
                       "UPDATE audit_event SET subject='EVIL' WHERE seq=1",
                       NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(raw);
   char err[160];
   assert(audit_worm_verify_file(sealed, err, sizeof err) == -1);
   assert(strstr(err, "seq 1") != NULL);
   printf("  test_sealed_snapshot_tamper_detected: ok (%s)\n", err);
}

int main(void)
{
   mk_tmpdir();
   test_append_and_chain();
   test_worm_triggers_block_mutation();
   test_cross_store_determinism();
   test_tamper_detected_past_triggers();
   test_checkpoint_and_verify_status();
   test_checkpoint_bound_to_chain_key();
   test_seal_snapshot_verifies();
   test_sealed_snapshot_tamper_detected();
   printf("all tests passed\n");
   return 0;
}
