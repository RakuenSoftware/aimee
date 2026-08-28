/* test_audit_worm.c: S0 WORM audit store — chain integrity, gap-free seq,
 * WORM triggers, cross-store determinism, and crypto tamper detection. */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h> /* FS_IOC_GETFLAGS/SETFLAGS, FS_IMMUTABLE_FL */
#include <sys/ioctl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <aimee/audit/audit_worm.h>
#include "cJSON.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char g_dir[256];

static void mk_tmpdir(void)
{
   snprintf(g_dir, sizeof g_dir, "%s/worm_test_XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_dir) != NULL);
}

/* audit_worm_seal() makes each sealed snapshot OS-immutable, which is the point
 * of a WORM store -- and it means this test cannot simply leave its directory
 * behind. Where the process holds CAP_LINUX_IMMUTABLE the seal really takes, and
 * nothing (not even root) can unlink the file until FS_IMMUTABLE_FL is cleared:
 * the suite's own tmpdir cleanup then fails with "Operation not permitted" and
 * takes the whole run down with it. CI containers usually lack the capability,
 * so the seal degrades to crypto-only there and the leak stays invisible until
 * the suite runs somewhere privileged.
 *
 * Unseal what we sealed, then remove the directory. Best-effort throughout: a
 * filesystem without the flag, or a process without the capability, never sealed
 * anything in the first place. */
static void unseal(const char *path)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return;
   int flags = 0;
   if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && (flags & FS_IMMUTABLE_FL))
   {
      flags &= ~FS_IMMUTABLE_FL;
      (void)ioctl(fd, FS_IOC_SETFLAGS, &flags);
   }
   close(fd);
}

static void rm_tmpdir(void)
{
   if (!g_dir[0])
      return;
   DIR *d = opendir(g_dir);
   if (d)
   {
      struct dirent *e;
      while ((e = readdir(d)) != NULL)
      {
         if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
         char child[512];
         snprintf(child, sizeof child, "%s/%s", g_dir, e->d_name);
         unseal(child);
         if (remove(child) != 0)
         {
            /* A nested directory: the audit/ subdir the sealed snapshots live in. */
            DIR *sub = opendir(child);
            if (sub)
            {
               struct dirent *se;
               while ((se = readdir(sub)) != NULL)
               {
                  if (strcmp(se->d_name, ".") == 0 || strcmp(se->d_name, "..") == 0)
                     continue;
                  char leaf[1024];
                  snprintf(leaf, sizeof leaf, "%s/%s", child, se->d_name);
                  unseal(leaf);
                  (void)remove(leaf);
               }
               closedir(sub);
               (void)rmdir(child);
            }
         }
      }
      closedir(d);
   }
   (void)rmdir(g_dir);
   g_dir[0] = '\0';
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
      /* The timestamp is a hash input (audit_worm_row_hash_v2), and plain
       * audit_worm_append stamps it from time(NULL) at one-second granularity.
       * Appending to the two stores in sequence therefore produces different
       * hashes whenever the pair straddles a second boundary -- rare on an idle
       * machine, routine on a loaded CI runner, which is where this failed. The
       * claim under test is that identical INPUTS hash identically across stores,
       * so pin the timestamp rather than race the clock. Plain append keeps its
       * coverage from the other cases in this file. */
      assert(audit_worm_append_idempotent("determinism:1", "2026-08-26T01:02:03Z", "primary", "u1",
                                          "tool.read", "v1-fixed", "allow", "{\"x\":1}",
                                          NULL) == 0);
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

static void test_timestamp_tamper_detected(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/ts.db", g_dir);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u1", "tool.read", "v1-1", "allow", "{}") == 0);
   audit_worm_close();
   sqlite3 *raw = NULL;
   assert(sqlite3_open(path, &raw) == SQLITE_OK);
   assert(sqlite3_exec(raw,
                       "DROP TRIGGER audit_event_no_update;"
                       "UPDATE audit_event SET ts='1999-01-01T00:00:00Z' WHERE seq=1",
                       NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(raw);
   assert(audit_worm_init_at(path) == 0);
   char err[128];
   assert(audit_worm_verify_chain(err, sizeof err) == -1);
   audit_worm_close();
   printf("  test_timestamp_tamper_detected: ok (%s)\n", err);
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

/* Process admission is shared by the server and KB worker: a fresh store is
 * accepted, an intact tail is checkpointed to GREEN, and offline tampering
 * prevents startup before either process accepts work. */
static void test_startup_verify_fails_closed(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/startup.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   char err[160] = "";
   long head = -1, ckpt = -1;
   assert(audit_worm_startup_verify(err, sizeof err, &head, &ckpt) == 0);
   assert(head == 0 && ckpt == 0);

   assert(audit_worm_append("server", "startup", "server.ready", "", "ok", "{}") == 0);
   assert(audit_worm_startup_verify(err, sizeof err, &head, &ckpt) == 0);
   assert(head == 2 && ckpt == 2);
   assert(audit_worm_verify(err, sizeof err, NULL, NULL) == AUDIT_WORM_VERIFY_GREEN);
   audit_worm_close();

   sqlite3 *raw = NULL;
   assert(sqlite3_open(path, &raw) == SQLITE_OK);
   assert(sqlite3_exec(raw,
                       "DROP TRIGGER audit_event_no_update;"
                       "UPDATE audit_event SET subject='offline-tamper' WHERE seq=1",
                       NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(raw);

   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_startup_verify(err, sizeof err, NULL, NULL) == -1);
   assert(strstr(err, "seq 1") != NULL);
   audit_worm_close();
   printf("  test_startup_verify_fails_closed: ok (%s)\n", err);
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

/* The WORM-backed Logs read returns the newest rows (seq DESC), paginated. */
static void test_read_page(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/page.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   for (int i = 0; i < 5; i++)
   {
      char s[16];
      snprintf(s, sizeof s, "v1-%d", i);
      assert(audit_worm_append("primary", "u", "tool.read", s, "allow", "{}") == 0);
   }
   long total = 0;
   cJSON *page = audit_worm_read_page(0, 2, &total);
   assert(total == 5);
   assert(cJSON_GetArraySize(page) == 2);
   cJSON *r0 = cJSON_GetArrayItem(page, 0); /* newest first: seq 5 */
   assert((int)cJSON_GetNumberValue(cJSON_GetObjectItem(r0, "seq")) == 5);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(r0, "subject")), "v1-4") == 0);
   cJSON_Delete(page);
   cJSON *page2 = audit_worm_read_page(2, 2, &total); /* offset: seq 3 then 2 */
   assert((int)cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(page2, 0), "seq")) == 3);
   cJSON_Delete(page2);
   audit_worm_close();
   printf("  test_read_page: ok\n");
}

/* A metric.snapshot row records the verdict-mix and is hash-chained like any row. */
static void test_metric_snapshot(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/metric.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(audit_worm_append("primary", "u", "tool.write", "v1-2", "block", "{}") == 0);
   assert(audit_worm_metric_snapshot() == 0);
   assert(audit_worm_verify_chain(NULL, 0) == 0);
   long total = 0;
   cJSON *page = audit_worm_read_page(0, 1, &total);
   cJSON *r0 = cJSON_GetArrayItem(page, 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(r0, "action")), "metric.snapshot") == 0);
   const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(r0, "detail"));
   assert(strstr(d, "\"allow\":1") && strstr(d, "\"block\":1"));
   cJSON_Delete(page);
   audit_worm_close();
   printf("  test_metric_snapshot: ok\n");
}

/* An oversized detail is capped with a marker; the chain stays intact. */
static void test_detail_capped(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/cap.db", g_dir);
   setenv("AIMEE_HOME", g_dir, 1);
   assert(audit_worm_init_at(path) == 0);
   size_t n = AUDIT_WORM_DETAIL_MAX + 5000;
   char *big = malloc(n + 1);
   assert(big);
   memset(big, 'x', n);
   big[n] = '\0';
   assert(audit_worm_append("primary", "u", "tool.read", "v1-1", "allow", big) == 0);
   free(big);
   assert(audit_worm_verify_chain(NULL, 0) == 0);
   long total = 0;
   cJSON *page = audit_worm_read_page(0, 1, &total);
   const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(page, 0), "detail"));
   assert(strstr(d, "worm-truncated"));
   assert(strlen(d) < (size_t)AUDIT_WORM_DETAIL_MAX + 200);
   cJSON_Delete(page);
   audit_worm_close();
   printf("  test_detail_capped: ok\n");
}

/* A durable producer may retry after SQLite committed but before its delivery
 * acknowledgement committed. The stable event id converges on the original
 * row, while reusing an id for different evidence fails closed. */
static void test_idempotent_outbox_delivery(void)
{
   char path[300];
   snprintf(path, sizeof path, "%s/idempotent.db", g_dir);
   assert(audit_worm_init_at(path) == 0);
   long long first = 0, retry = 0;
   assert(audit_worm_append_idempotent("kb:41", "2026-08-26T01:02:03Z", "kb", "worker", "kb.query",
                                       "q1", "ok", "{}", &first) == 0);
   assert(first == 1);
   assert(audit_worm_append_idempotent("kb:41", "2026-08-26T01:02:03Z", "kb", "worker", "kb.query",
                                       "q1", "ok", "{}", &retry) == 0);
   assert(retry == first);
   assert(audit_worm_count() == 1);
   assert(audit_worm_append_idempotent("kb:41", "2026-08-26T01:02:03Z", "kb", "worker", "kb.query",
                                       "DIFFERENT", "ok", "{}", NULL) == -1);
   assert(audit_worm_count() == 1);
   assert(audit_worm_verify_chain(NULL, 0) == 0);
   audit_worm_close();

   /* event_id is retry metadata, but it is still evidence-bound: bypassing the
    * triggers to rewrite it must break verification. */
   sqlite3 *raw = NULL;
   assert(sqlite3_open(path, &raw) == SQLITE_OK);
   assert(sqlite3_exec(raw,
                       "DROP TRIGGER audit_event_no_update;"
                       "UPDATE audit_event SET event_id='kb:forged' WHERE seq=1",
                       NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(raw);
   assert(audit_worm_init_at(path) == 0);
   assert(audit_worm_verify_chain(NULL, 0) == -1);
   audit_worm_close();
   printf("  test_idempotent_outbox_delivery: ok\n");
}

/* Golden vector for ordinary rows in the shared server/KB SQLite implementation. */
static void test_cross_engine_vector(void)
{
   char h[65];
   audit_worm_row_hash(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{}",
                       AUDIT_WORM_GENESIS_PREV, h);
   assert(strcmp(h, "3c2adf68ae8f1b704780ffedd32522e06468e34a69205240fbb358a7122ff986") == 0);
   printf("  test_cross_engine_vector: ok\n");
}

int main(void)
{
   mk_tmpdir();
   assert(atexit(rm_tmpdir) == 0);
   test_cross_engine_vector();
   test_metric_snapshot();
   test_detail_capped();
   test_idempotent_outbox_delivery();
   test_read_page();
   test_append_and_chain();
   test_worm_triggers_block_mutation();
   test_cross_store_determinism();
   test_tamper_detected_past_triggers();
   test_timestamp_tamper_detected();
   test_checkpoint_and_verify_status();
   test_startup_verify_fails_closed();
   test_checkpoint_bound_to_chain_key();
   test_seal_snapshot_verifies();
   test_sealed_snapshot_tamper_detected();
   printf("all tests passed\n");
   return 0;
}
