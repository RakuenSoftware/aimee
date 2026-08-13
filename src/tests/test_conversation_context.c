/* test_conversation_context.c: unit tests for db1/conv_context.c and
 * conversation_context.c (Phase 1+2).
 *
 * Tests event recording, pending-event retrieval, chain insertion,
 * chain_id assignment, chain listing, stub search, state tracking, and
 * the Phase 2 budget-aware assembly (conv_ctx_assemble). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db1.h"
#include "conv_context.h"
#include "conversation_context.h"
#include "platform_test_util.h"

static void test_record_and_pending(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* Initially: no pending events */
   conv_tool_event_t events[16];
   int n = db1_conv_pending_events("sess-1", events, 16);
   assert(n == 0);

   /* Record two events */
   int64_t id1 = db1_conv_record_event("sess-1", "read_file", "{\"file_path\":\"src/foo.c\"}",
                                       "line1\nline2\n", 12);
   int64_t id2 = db1_conv_record_event("sess-1", "bash", "{\"command\":\"make\"}", "ok\n", 3);
   assert(id1 > 0);
   assert(id2 > id1);

   n = db1_conv_pending_events("sess-1", events, 16);
   assert(n == 2);
   assert(events[0].id == id1);
   assert(strcmp(events[0].tool_name, "read_file") == 0);
   assert(events[1].id == id2);
   assert(strcmp(events[1].tool_name, "bash") == 0);

   /* Different session should have no events */
   n = db1_conv_pending_events("sess-other", events, 16);
   assert(n == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_chain_insert_and_assign(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   int64_t id1 = db1_conv_record_event("sess-2", "read_file", "{}", "result1", 7);
   int64_t id2 = db1_conv_record_event("sess-2", "bash", "{}", "result2", 7);
   assert(id1 > 0 && id2 > 0);

   /* Insert chain */
   int64_t cid = db1_conv_insert_chain("sess-2", id1, id2, "read_file,bash",
                                       "Tools: read_file,bash. Files: (none).", 14, 37);
   assert(cid > 0);

   /* Assign chain_id to events */
   int rc = db1_conv_set_chain_id(id1, id2, cid);
   assert(rc == 0);

   /* Pending events should now be empty */
   conv_tool_event_t events[16];
   int n = db1_conv_pending_events("sess-2", events, 16);
   assert(n == 0);

   /* Chain events should return both */
   n = db1_conv_chain_events(cid, events, 16);
   assert(n == 2);
   assert(events[0].id == id1);
   assert(events[1].id == id2);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_list_chains(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   int64_t a1 = db1_conv_record_event("sess-3", "tool_a", "{}", "ra", 2);
   int64_t a2 = db1_conv_record_event("sess-3", "tool_b", "{}", "rb", 2);
   int64_t c1 =
       db1_conv_insert_chain("sess-3", a1, a2, "tool_a,tool_b", "Tools: tool_a,tool_b.", 4, 21);
   assert(c1 > 0);
   db1_conv_set_chain_id(a1, a2, c1);

   int64_t b1 = db1_conv_record_event("sess-3", "tool_c", "{}", "rc", 2);
   int64_t b2 = db1_conv_record_event("sess-3", "tool_d", "{}", "rd", 2);
   int64_t c2 =
       db1_conv_insert_chain("sess-3", b1, b2, "tool_c,tool_d", "Tools: tool_c,tool_d.", 4, 21);
   assert(c2 > c1);
   db1_conv_set_chain_id(b1, b2, c2);

   conv_tool_chain_t chains[16];
   int n = db1_conv_list_chains("sess-3", chains, 16);
   assert(n == 2);
   /* Ordered by id DESC, so c2 first */
   assert(chains[0].id == c2);
   assert(chains[1].id == c1);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_search_chains(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   int64_t e1 = db1_conv_record_event("sess-4", "read_file", "{}", "auth middleware code", 20);
   int64_t e2 = db1_conv_record_event("sess-4", "bash", "{}", "test pass", 9);
   int64_t c1 = db1_conv_insert_chain(
       "sess-4", e1, e2, "read_file,bash",
       "Tools: read_file,bash. Files: middleware.c. auth header fix.", 29, 60);
   db1_conv_set_chain_id(e1, e2, c1);

   int64_t e3 = db1_conv_record_event("sess-4", "bash", "{}", "deploy output", 13);
   int64_t e4 = db1_conv_record_event("sess-4", "bash", "{}", "done", 4);
   int64_t c2 =
       db1_conv_insert_chain("sess-4", e3, e4, "bash", "Tools: bash. deploy pipeline run.", 17, 33);
   db1_conv_set_chain_id(e3, e4, c2);

   conv_tool_chain_t out[16];
   int n;

   /* Search for "auth" should find c1 */
   n = db1_conv_search_chains("sess-4", "auth", out, 16);
   assert(n == 1);
   assert(out[0].id == c1);

   /* Search for "deploy" should find c2 */
   n = db1_conv_search_chains("sess-4", "deploy", out, 16);
   assert(n == 1);
   assert(out[0].id == c2);

   /* Search for "bash" should find both */
   n = db1_conv_search_chains("sess-4", "bash", out, 16);
   assert(n == 2);

   /* Empty result for unknown term */
   n = db1_conv_search_chains("sess-4", "zzznomatch", out, 16);
   assert(n == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_state_get_update(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* No state yet: get returns -1 (not found) */
   int64_t last_ev = -99;
   int cc = -99, ec = -99;
   int rc = db1_conv_state_get("sess-5", &last_ev, &cc, &ec);
   assert(rc != 0); /* not found */

   /* Update state */
   rc = db1_conv_state_update("sess-5", 42, 3, 10);
   assert(rc == 0);

   /* Now get should succeed */
   last_ev = -99;
   cc = -99;
   ec = -99;
   rc = db1_conv_state_get("sess-5", &last_ev, &cc, &ec);
   assert(rc == 0);
   assert(last_ev == 42);
   assert(cc == 3);
   assert(ec == 10);

   /* Upsert again */
   rc = db1_conv_state_update("sess-5", 55, 4, 15);
   assert(rc == 0);
   rc = db1_conv_state_get("sess-5", &last_ev, &cc, &ec);
   assert(rc == 0);
   assert(last_ev == 55);
   assert(cc == 4);
   assert(ec == 15);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

/* ── Phase 2: conv_ctx_assemble (with virtual_context disabled) ───────────── */

static void test_assemble_disabled(const char *path)
{
   platform_test_remove_sqlite(path);

   /* The feature is on by default since the rollout flip, so disable it
    * explicitly via config to exercise the disabled path. */
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee_vc_off_XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   char cfgpath[256];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", tmpdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "session:\n  virtual_context:\n    enabled: false\n");
   fclose(fp);

   const char *old_home = getenv("AIMEE_HOME");
   setenv("AIMEE_HOME", tmpdir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   assert(db1_init(path) == 0);

   /* Insert a chain directly */
   int64_t e1 = db1_conv_record_event("sess-a1", "read_file", "{}", "content", 7);
   int64_t e2 = db1_conv_record_event("sess-a1", "bash", "{}", "ok", 2);
   int64_t c1 = db1_conv_insert_chain("sess-a1", e1, e2, "read_file,bash",
                                      "Tools: read_file,bash. Files: foo.c.", 9, 36);
   assert(c1 > 0);
   db1_conv_set_chain_id(e1, e2, c1);

   /* conv_ctx_assemble returns NULL when virtual_context is disabled */
   char *out = conv_ctx_assemble("sess-a1", NULL, 1024);
   assert(out == NULL);

   if (old_home)
      setenv("AIMEE_HOME", old_home, 1);
   else
      unsetenv("AIMEE_HOME");
   unsetenv("AIMEE_NO_CACHE");
   unlink(cfgpath);
   rmdir(tmpdir);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_assemble_no_chains(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* Session has no chains: assemble returns NULL */
   char *out = conv_ctx_assemble("sess-empty", NULL, 1024);
   assert(out == NULL);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

/* ── Integration: session crosses compaction threshold ───────────────────── */

static void test_integration_compaction_threshold(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   const char *sid = "sess-integ-1";
   static const char BIG[] =
       "line1: lots of content here\nline2: more content\nline3: even more content\n"
       "line4: function body\nline5: closing brace\n";
   int big_sz = (int)strlen(BIG);

   /* Record 10 events to guarantee 2 full batches of 5 */
   for (int i = 0; i < 10; i++)
   {
      char input[64];
      snprintf(input, sizeof(input), "{\"file_path\":\"src/mod%d.c\"}", i);
      int64_t id =
          db1_conv_record_event(sid, i % 2 == 0 ? "read_file" : "bash", input, BIG, big_sz);
      assert(id > 0);
   }

   int chains_created = conv_ctx_flush_pending(sid);
   assert(chains_created > 0);

   conv_tool_chain_t chains[16];
   int n = db1_conv_list_chains(sid, chains, 16);
   assert(n > 0);

   /* Pending queue is cleared (fewer than one batch-minimum remain) */
   conv_tool_event_t pending[32];
   int pending_n = db1_conv_pending_events(sid, pending, 32);
   assert(pending_n < 2);

   /* Each chain is compressed: stub smaller than raw */
   for (int i = 0; i < n; i++)
   {
      assert(chains[i].stub_bytes > 0);
      assert(chains[i].raw_bytes > 0);
      assert(chains[i].stub_bytes < chains[i].raw_bytes);
   }

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

/* ── Integration: assembly returns content matching a query ─────────────── */

static void test_integration_assemble_retrieval(const char *path)
{
   platform_test_remove_sqlite(path);

   /* Temp config dir enabling virtual_context */
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee_vc_test_XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);
   char cfgpath[256];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", tmpdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "session:\n  virtual_context:\n    enabled: true\n");
   fclose(fp);

   const char *old_home = getenv("AIMEE_HOME");
   setenv("AIMEE_HOME", tmpdir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   assert(db1_init(path) == 0);

   const char *sid = "sess-asm-vc";
   int64_t e1 = db1_conv_record_event(sid, "read_file", "{\"file_path\":\"src/auth.c\"}",
                                      "auth header validation code", 26);
   int64_t e2 = db1_conv_record_event(sid, "bash", "{\"command\":\"make test\"}", "tests pass", 10);
   assert(e1 > 0 && e2 > 0);

   int chains = conv_ctx_flush_pending(sid);
   assert(chains > 0);

   char *out = conv_ctx_assemble(sid, "auth", 2048);
   assert(out != NULL);
   /* Output should contain chain summary information */
   assert(strlen(out) > 0);
   free(out);

   /* Cleanup */
   if (old_home)
      setenv("AIMEE_HOME", old_home, 1);
   else
      unsetenv("AIMEE_HOME");
   unsetenv("AIMEE_NO_CACHE");
   unlink(cfgpath);
   rmdir(tmpdir);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

/* ── Failure injection ───────────────────────────────────────────────────── */

static void test_failure_null_result(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* NULL result should be treated as empty — no crash */
   int64_t id1 = db1_conv_record_event("sess-fn-1", "bash", "{\"command\":\"make\"}", NULL, 0);
   assert(id1 > 0);
   int64_t id2 = db1_conv_record_event("sess-fn-1", "bash", "{\"command\":\"ls\"}", "", 0);
   assert(id2 > id1);

   /* Flush with sparse results should not crash */
   int rc = conv_ctx_flush_pending("sess-fn-1");
   assert(rc >= 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_failure_malformed_input(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* Malformed JSON in tool_input — should not crash; file path extraction silently fails */
   int64_t id1 = db1_conv_record_event("sess-fm-1", "read_file", "NOT_VALID_JSON", "result", 6);
   assert(id1 > 0);
   int64_t id2 = db1_conv_record_event("sess-fm-1", "read_file", "{invalid", "result2", 7);
   assert(id2 > id1);

   int rc = conv_ctx_flush_pending("sess-fm-1");
   assert(rc >= 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_failure_expand_nonexistent(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   /* Expanding a chain_id that doesn't exist returns 0 events, no crash */
   conv_tool_event_t events[16];
   int n = db1_conv_chain_events(999999, events, 16);
   assert(n == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

int main(void)
{
   printf("conv_context: ");

   char path[256];
   snprintf(path, sizeof path, "%s/test_conv_context_XXXXXX.db", platform_tmpdir());
   int fd = mkstemps(path, 3);
   if (fd >= 0)
      close(fd);

   test_record_and_pending(path);
   test_chain_insert_and_assign(path);
   test_list_chains(path);
   test_search_chains(path);
   test_state_get_update(path);
   test_assemble_disabled(path);
   test_assemble_no_chains(path);
   test_integration_compaction_threshold(path);
   test_integration_assemble_retrieval(path);
   test_failure_null_result(path);
   test_failure_malformed_input(path);
   test_failure_expand_nonexistent(path);

   printf("ok\n");
   return 0;
}
