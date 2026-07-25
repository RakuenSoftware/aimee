/* test_delegate_backend_local.c: round-trip tests for the local
 * backend's acquire/release/exec/cwd surface. Uses a temp
 * XDG_CACHE_HOME so the workspace is isolated from the user's real
 * cache. */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aimee/delegates/delegate_backend_local.h>

static char g_tmp_root[256];

static void setup_tmp_cache(void)
{
   snprintf(g_tmp_root, sizeof(g_tmp_root), "/tmp/aimee-backend-test-%d", (int)getpid());
   mkdir(g_tmp_root, 0700);
   setenv("XDG_CACHE_HOME", g_tmp_root, 1);
}

static int dir_exists(const char *p)
{
   struct stat s;
   return stat(p, &s) == 0 && S_ISDIR(s.st_mode);
}

static void test_register_puts_local_in_registry(void)
{
   delegate_backend_reset_for_test();
   assert(delegate_backend_register_local() == 0);
   delegate_backend_t *b = delegate_backend_lookup("local");
   assert(b != NULL);
   assert(b == delegate_backend_local_get());
   /* Second call is a no-op (duplicate-name rejection). */
   assert(delegate_backend_register_local() == -1);
   printf("  PASS: test_register_puts_local_in_registry\n");
}

static void test_acquire_creates_workspace(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-acquire-1", NULL, &state) == 0);
   assert(state != NULL);
   char expected[512];
   snprintf(expected, sizeof(expected), "%s/aimee/delegate/task-acquire-1", g_tmp_root);
   assert(dir_exists(expected));
   b->release(b, state, 0);
   assert(!dir_exists(expected));
   printf("  PASS: test_acquire_creates_workspace\n");
}

static void test_release_hibernate_keeps_workspace(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-hibernate-1", NULL, &state) == 0);
   char expected[512];
   snprintf(expected, sizeof(expected), "%s/aimee/delegate/task-hibernate-1", g_tmp_root);
   assert(dir_exists(expected));
   b->release(b, state, 1); /* hibernate=1 -> keep on disk */
   assert(dir_exists(expected));

   /* Re-acquire same task_id resumes the same workspace. */
   void *state2 = NULL;
   assert(b->acquire(b, "task-hibernate-1", NULL, &state2) == 0);
   assert(dir_exists(expected));
   b->release(b, state2, 0);
   assert(!dir_exists(expected));
   printf("  PASS: test_release_hibernate_keeps_workspace\n");
}

static void test_exec_runs_command_and_captures_stdout(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-exec-1", NULL, &state) == 0);

   char out[4096] = {0};
   char err[4096] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "echo hello-world", 5000, &r) == 0);
   assert(r.exit_code == 0);
   assert(strcmp(out, "hello-world\n") == 0);
   b->release(b, state, 0);
   printf("  PASS: test_exec_runs_command_and_captures_stdout\n");
}

static void test_exec_propagates_nonzero_exit(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-exec-2", NULL, &state) == 0);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "exit 7", 5000, &r) == 0);
   assert(r.exit_code == 7);
   b->release(b, state, 0);
   printf("  PASS: test_exec_propagates_nonzero_exit\n");
}

static void test_exec_captures_stderr(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-exec-3", NULL, &state) == 0);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "echo to-stderr 1>&2", 5000, &r) == 0);
   assert(strcmp(err, "to-stderr\n") == 0);
   b->release(b, state, 0);
   printf("  PASS: test_exec_captures_stderr\n");
}

static void test_exec_runs_in_set_cwd(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-cwd-1", NULL, &state) == 0);

   /* Default cwd is the workspace; pwd echoes it back. */
   char *cwd = NULL;
   assert(b->get_cwd(b, state, &cwd) == 0);
   assert(cwd != NULL);
   assert(strstr(cwd, "task-cwd-1") != NULL);

   char out[512] = {0}, err[512] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "pwd", 5000, &r) == 0);
   /* pwd output has a trailing newline; cwd does not. */
   assert(strncmp(out, cwd, strlen(cwd)) == 0);
   free(cwd);

   /* Changing cwd to a stable path that must exist on every Linux box. */
   assert(b->set_cwd(b, state, "/tmp") == 0);
   memset(out, 0, sizeof(out));
   assert(b->exec(b, state, "pwd", 5000, &r) == 0);
   assert(strcmp(out, "/tmp\n") == 0);
   b->release(b, state, 0);
   printf("  PASS: test_exec_runs_in_set_cwd\n");
}

static void test_exec_cwd_persists_across_calls(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-cwd-persist", NULL, &state) == 0);

   char out[512] = {0}, err[512] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   /* First call: cd into /tmp inside the user command. The wrapper
    * captures pwd to .cwd and the next exec resumes there. */
   assert(b->exec(b, state, "cd /tmp && pwd", 5000, &r) == 0);
   assert(r.exit_code == 0);
   assert(strcmp(out, "/tmp\n") == 0);

   /* Second call with no cd: should run from /tmp because of the
    * snapshot, even though we never called set_cwd. */
   memset(out, 0, sizeof(out));
   assert(b->exec(b, state, "pwd", 5000, &r) == 0);
   assert(r.exit_code == 0);
   assert(strcmp(out, "/tmp\n") == 0);

   /* And get_cwd reflects the new state. */
   char *cwd = NULL;
   assert(b->get_cwd(b, state, &cwd) == 0);
   assert(strcmp(cwd, "/tmp") == 0);
   free(cwd);
   b->release(b, state, 0);
   printf("  PASS: test_exec_cwd_persists_across_calls\n");
}

static void test_exec_smoke_test_from_proposal(void)
{
   /* The shared smoke test from delegate-execution-backends.md
    * acceptance criteria: `cd /tmp && pwd && echo done` returns
    * "/tmp\ndone\n" and cwd persists across two exec calls. */
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-smoke", NULL, &state) == 0);

   char out[512] = {0}, err[512] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "cd /tmp && pwd && echo done", 5000, &r) == 0);
   assert(r.exit_code == 0);
   assert(strcmp(out, "/tmp\ndone\n") == 0);

   /* cwd persists across the two exec calls. */
   memset(out, 0, sizeof(out));
   assert(b->exec(b, state, "pwd", 5000, &r) == 0);
   assert(strcmp(out, "/tmp\n") == 0);
   b->release(b, state, 0);
   printf("  PASS: test_exec_smoke_test_from_proposal\n");
}

static void test_exec_timeout_kills_child(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-timeout-1", NULL, &state) == 0);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   /* 5s sleep with 200ms budget: must terminate well under 5s. */
   assert(b->exec(b, state, "sleep 5; echo never", 200, &r) == 0);
   assert(r.latency_ms < 4000);
   /* SIGTERM-ed child reports exit_code -1 (no clean exit). */
   assert(r.exit_code != 0); /* may be -1 (signal) or other */
   assert(strstr(out, "never") == NULL);
   b->release(b, state, 0);
   printf("  PASS: test_exec_timeout_kills_child\n");
}

static void test_write_then_read_roundtrip(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-fileio-1", NULL, &state) == 0);

   assert(b->write_file(b, state, "hello.txt", "hello world\n") == 0);
   char *content = NULL;
   assert(b->read_file(b, state, "hello.txt", 0, 0, &content) == 0);
   assert(content != NULL);
   assert(strcmp(content, "hello world\n") == 0);
   free(content);
   b->release(b, state, 0);
   printf("  PASS: test_write_then_read_roundtrip\n");
}

static void test_read_with_offset_and_limit(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-fileio-2", NULL, &state) == 0);
   assert(b->write_file(b, state, "data.txt", "0123456789abcdef") == 0);

   /* offset=4, limit=5 → "45678" */
   char *content = NULL;
   assert(b->read_file(b, state, "data.txt", 4, 5, &content) == 0);
   assert(strcmp(content, "45678") == 0);
   free(content);
   b->release(b, state, 0);
   printf("  PASS: test_read_with_offset_and_limit\n");
}

static void test_read_missing_file_returns_error(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-fileio-3", NULL, &state) == 0);
   char *content = (char *)0x1;
   assert(b->read_file(b, state, "nope.txt", 0, 0, &content) == -1);
   assert(content == NULL);
   b->release(b, state, 0);
   printf("  PASS: test_read_missing_file_returns_error\n");
}

static void test_write_creates_parent_dirs(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-fileio-4", NULL, &state) == 0);
   assert(b->write_file(b, state, "deep/nested/path/file.txt", "ok") == 0);
   char *content = NULL;
   assert(b->read_file(b, state, "deep/nested/path/file.txt", 0, 0, &content) == 0);
   assert(strcmp(content, "ok") == 0);
   free(content);
   b->release(b, state, 0);
   printf("  PASS: test_write_creates_parent_dirs\n");
}

static void test_path_validation_rejects_escapes(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-fileio-5", NULL, &state) == 0);

   /* Absolute paths rejected. */
   assert(b->write_file(b, state, "/etc/passwd", "x") == -1);
   /* `..` segments rejected. */
   assert(b->write_file(b, state, "../escape.txt", "x") == -1);
   assert(b->write_file(b, state, "ok/../../escape.txt", "x") == -1);
   /* Read also rejects. */
   char *content = (char *)0x1;
   assert(b->read_file(b, state, "../etc/passwd", 0, 0, &content) == -1);
   assert(content == NULL);
   /* list_dir rejects too. */
   char **entries = (char **)0x1;
   assert(b->list_dir(b, state, "../", &entries) == -1);
   assert(entries == NULL);
   b->release(b, state, 0);
   printf("  PASS: test_path_validation_rejects_escapes\n");
}

static void test_list_dir_returns_entries(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-list-1", NULL, &state) == 0);

   /* Empty workspace dir initially has no entries (well, it may have
    * .snap.sh/.cwd from a prior exec — for a fresh acquire it's
    * empty). */
   char **entries = NULL;
   assert(b->list_dir(b, state, ".", &entries) == 0);
   assert(entries != NULL);
   assert(entries[0] == NULL);
   free(entries);

   /* Add a couple of files; list_dir picks them up. */
   assert(b->write_file(b, state, "a.txt", "a") == 0);
   assert(b->write_file(b, state, "b.txt", "b") == 0);
   int n = b->list_dir(b, state, ".", &entries);
   assert(n == 2);
   /* Order is filesystem-defined; just verify both are present. */
   int saw_a = 0, saw_b = 0;
   for (int i = 0; entries[i]; i++)
   {
      if (strcmp(entries[i], "a.txt") == 0)
         saw_a = 1;
      if (strcmp(entries[i], "b.txt") == 0)
         saw_b = 1;
      free(entries[i]);
   }
   free(entries);
   assert(saw_a && saw_b);

   b->release(b, state, 0);
   printf("  PASS: test_list_dir_returns_entries\n");
}

static void test_list_dir_missing_path_returns_error(void)
{
   setup_tmp_cache();
   delegate_backend_t *b = delegate_backend_local_get();
   void *state = NULL;
   assert(b->acquire(b, "task-list-2", NULL, &state) == 0);
   char **entries = (char **)0x1;
   assert(b->list_dir(b, state, "no-such-dir", &entries) == -1);
   assert(entries == NULL);
   b->release(b, state, 0);
   printf("  PASS: test_list_dir_missing_path_returns_error\n");
}

int main(void)
{
   printf("delegate_backend_local:\n");
   test_register_puts_local_in_registry();
   test_acquire_creates_workspace();
   test_release_hibernate_keeps_workspace();
   test_exec_runs_command_and_captures_stdout();
   test_exec_propagates_nonzero_exit();
   test_exec_captures_stderr();
   test_exec_runs_in_set_cwd();
   test_exec_cwd_persists_across_calls();
   test_exec_smoke_test_from_proposal();
   test_exec_timeout_kills_child();
   test_write_then_read_roundtrip();
   test_read_with_offset_and_limit();
   test_read_missing_file_returns_error();
   test_write_creates_parent_dirs();
   test_path_validation_rejects_escapes();
   test_list_dir_returns_entries();
   test_list_dir_missing_path_returns_error();
   printf("ok\n");
   return 0;
}
