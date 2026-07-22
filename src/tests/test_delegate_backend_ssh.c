/* test_delegate_backend_ssh.c: registry membership, ssh-command
 * builder, and fake-ssh round trips. */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aimee/delegates/delegate_backend_ssh.h>

/* Forward decl — definition lives below the first cluster of tests
 * that use it, with the rest of the fixture-using cases. */
static const char *write_fake_ssh_fixture(int succeed);

static void test_register_puts_ssh_in_registry(void)
{
   delegate_backend_reset_for_test();
   assert(delegate_backend_register_ssh() == 0);
   delegate_backend_t *b = delegate_backend_lookup("ssh");
   assert(b != NULL);
   assert(b == delegate_backend_ssh_get());
   /* Idempotent: second call rejected by the registry. */
   assert(delegate_backend_register_ssh() == -1);
   /* Vtable slots are wired (no NULL function pointers). */
   assert(b->acquire != NULL);
   assert(b->release != NULL);
   assert(b->exec != NULL);
   assert(b->read_file != NULL);
   assert(b->write_file != NULL);
   assert(b->list_dir != NULL);
   assert(b->get_cwd != NULL);
   assert(b->set_cwd != NULL);
   printf("  PASS: test_register_puts_ssh_in_registry\n");
}

static void test_file_ops_reject_null_state(void)
{
   delegate_backend_reset_for_test();
   assert(delegate_backend_register_ssh() == 0);
   delegate_backend_t *b = delegate_backend_ssh_get();
   b->release(b, NULL, 0); /* tolerates NULL */
   char *out = (char *)0x1;
   assert(b->read_file(b, NULL, "x", 0, 0, &out) == -1);
   assert(out == NULL);
   assert(b->write_file(b, NULL, "p", "c") == -1);
   char **entries = (char **)0x1;
   assert(b->list_dir(b, NULL, ".", &entries) == -1);
   assert(entries == NULL);
   printf("  PASS: test_file_ops_reject_null_state\n");
}

static void test_exec_runs_through_fake_ssh(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   const char *fixture = write_fake_ssh_fixture(1);
   setenv("AIMEE_SSH_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   cfg.host = "fake@nowhere";
   void *state = NULL;
   assert(b->acquire(b, "task-ssh-exec-1", &cfg, &state) == 0);

   char out[4096] = {0}, err[4096] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   /* The fixture's exec mode runs the last argv element locally.
    * That argv element from build_exec_command is:
    *   echo '<b64>' | base64 -d | bash
    * which decodes to our actual user command. */
   assert(b->exec(b, state, "echo hello-world", 5000, &r) == 0);
   assert(r.exit_code == 0);
   assert(strcmp(out, "hello-world\n") == 0);
   b->release(b, state, 0);

   unlink(fixture);
   unsetenv("AIMEE_SSH_BIN");
   printf("  PASS: test_exec_runs_through_fake_ssh\n");
}

static void test_exec_propagates_nonzero_exit(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   const char *fixture = write_fake_ssh_fixture(1);
   setenv("AIMEE_SSH_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   cfg.host = "h";
   void *state = NULL;
   assert(b->acquire(b, "task-ssh-exec-2", &cfg, &state) == 0);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "exit 7", 5000, &r) == 0);
   assert(r.exit_code == 7);
   b->release(b, state, 0);

   unlink(fixture);
   unsetenv("AIMEE_SSH_BIN");
   printf("  PASS: test_exec_propagates_nonzero_exit\n");
}

static void test_exec_set_cwd_prefixes_subsequent_calls(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   const char *fixture = write_fake_ssh_fixture(1);
   setenv("AIMEE_SSH_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   cfg.host = "h";
   void *state = NULL;
   assert(b->acquire(b, "task-ssh-cwd", &cfg, &state) == 0);

   /* set_cwd then exec pwd should report the chosen dir because
    * exec prefixes `cd <cwd> &&`. /tmp exists everywhere. */
   assert(b->set_cwd(b, state, "/tmp") == 0);
   char out[512] = {0}, err[512] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "pwd", 5000, &r) == 0);
   assert(strcmp(out, "/tmp\n") == 0);

   /* get_cwd reflects the set value. */
   char *cwd = NULL;
   assert(b->get_cwd(b, state, &cwd) == 0);
   assert(strcmp(cwd, "/tmp") == 0);
   free(cwd);
   b->release(b, state, 0);

   unlink(fixture);
   unsetenv("AIMEE_SSH_BIN");
   printf("  PASS: test_exec_set_cwd_prefixes_subsequent_calls\n");
}

/* Write a fake-ssh fixture script to /tmp and return its path.
 * Three behaviors based on argv:
 *   1. -M present  → master mode: touch ControlPath= file (if succeed=1), exit
 *   2. -O exit     → graceful master shutdown, exit 0
 *   3. otherwise   → exec mode: run the LAST argv element through bash so
 *                    "remote" exec calls actually execute locally. Lets the
 *                    test exercise the full ssh_exec wiring without sshd. */
static const char *write_fake_ssh_fixture(int succeed)
{
   static char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-fake-ssh-%d-%d.sh", (int)getpid(), succeed);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f,
           "#!/bin/bash\n"
           "is_master=0\n"
           "is_exit=0\n"
           "ctrl_path=\"\"\n"
           "for arg in \"$@\"; do\n"
           "  case \"$arg\" in\n"
           "    -M) is_master=1;;\n"
           "    -O) is_exit=1;;\n"
           "    ControlPath=*) ctrl_path=\"${arg#ControlPath=}\";;\n"
           "  esac\n"
           "done\n"
           "if [ $is_master -eq 1 ]; then\n"
           "  [ %d -eq 1 ] && [ -n \"$ctrl_path\" ] && touch \"$ctrl_path\"\n"
           "  exit 0\n"
           "fi\n"
           "if [ $is_exit -eq 1 ]; then\n"
           "  [ -n \"$ctrl_path\" ] && rm -f \"$ctrl_path\"\n"
           "  exit 0\n"
           "fi\n"
           "# exec mode: run the last argv element through bash locally.\n"
           "cmd=\"${@: -1}\"\n"
           "exec bash -c \"$cmd\"\n",
           succeed);
   fclose(f);
   chmod(path, 0700);
   return path;
}

static void test_acquire_succeeds_with_fake_ssh(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   const char *fixture = write_fake_ssh_fixture(1);
   setenv("AIMEE_SSH_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   cfg.host = "fake@nowhere";
   void *state = NULL;
   assert(b->acquire(b, "task-ssh-acq-1", &cfg, &state) == 0);
   assert(state != NULL);
   /* Release should clean up the multiplex socket. The fake fixture
    * exits 0 for the rm/exit calls too, so this is a smoke check
    * that release() doesn't crash. */
   b->release(b, state, 0);

   unlink(fixture);
   unsetenv("AIMEE_SSH_BIN");
   printf("  PASS: test_acquire_succeeds_with_fake_ssh\n");
}

static void test_acquire_fails_when_socket_never_appears(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   /* succeed=0: fixture exits 0 but does NOT touch the socket file.
    * acquire's wait_for_socket then times out and acquire returns -1. */
   const char *fixture = write_fake_ssh_fixture(0);
   setenv("AIMEE_SSH_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   cfg.host = "fake@nowhere";
   void *state = (void *)0x1;
   /* Note: acquire's internal wait timeout is 5s in production. To
    * keep the test under that, we monkey-wait a short time and
    * accept that this test takes ~5s. (Acceptable cost for the
    * integration smoke; alternative would be a shorter-timeout
    * variant of acquire which complicates the API.) */
   int rc = b->acquire(b, "task-ssh-acq-fail", &cfg, &state);
   assert(rc == -1);
   assert(state == NULL);

   unlink(fixture);
   unsetenv("AIMEE_SSH_BIN");
   printf("  PASS: test_acquire_fails_when_socket_never_appears\n");
}

static void test_acquire_rejects_invalid_args(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   delegate_backend_config_t cfg = {0};
   cfg.host = "h";
   void *state = (void *)0x1;
   /* Empty task_id rejected. */
   assert(b->acquire(b, "", &cfg, &state) == -1);
   assert(state == NULL);
   /* NULL cfg rejected. */
   state = (void *)0x1;
   assert(b->acquire(b, "task", NULL, &state) == -1);
   assert(state == NULL);
   /* cfg with no host rejected. */
   delegate_backend_config_t empty_cfg = {0};
   state = (void *)0x1;
   assert(b->acquire(b, "task", &empty_cfg, &state) == -1);
   assert(state == NULL);
   printf("  PASS: test_acquire_rejects_invalid_args\n");
}

static void test_build_exec_command_basic(void)
{
   char *cmd = NULL;
   assert(delegate_backend_ssh_build_exec_command("/tmp/aimee-ssh.sock", "user@pve", "echo hello",
                                                  &cmd) == 0);
   assert(cmd != NULL);
   /* Spot-check: ControlMaster socket path appears, BatchMode is set,
    * keepalive is configured, host is in there, and the script is
    * base64-encoded (so single-quotes inside it survive ssh's shell). */
   assert(strstr(cmd, "ssh -S /tmp/aimee-ssh.sock") != NULL);
   assert(strstr(cmd, "ControlPath=/tmp/aimee-ssh.sock") != NULL);
   assert(strstr(cmd, "BatchMode=yes") != NULL);
   assert(strstr(cmd, "ServerAliveInterval=30") != NULL);
   assert(strstr(cmd, "user@pve") != NULL);
   assert(strstr(cmd, "base64 -d | bash") != NULL);
   /* Raw user command must NOT appear directly — it's b64-encoded. */
   assert(strstr(cmd, "echo hello") == NULL);
   free(cmd);
   printf("  PASS: test_build_exec_command_basic\n");
}

static void test_build_exec_command_handles_special_chars(void)
{
   /* The whole point of base64-encoding the script is so quoting does
    * not matter. Pass a script with backticks, single quotes, and
    * dollar signs and confirm it round-trips through the encoding. */
   const char *evil = "echo 'with single quotes' && echo `backticks` && echo $HOME";
   char *cmd = NULL;
   assert(delegate_backend_ssh_build_exec_command("/sock", "h", evil, &cmd) == 0);
   /* The raw evil string MUST NOT appear; only the b64 form does. */
   assert(strstr(cmd, "with single quotes") == NULL);
   assert(strstr(cmd, "backticks") == NULL);
   /* The b64 envelope is intact. */
   assert(strstr(cmd, "base64 -d | bash") != NULL);
   free(cmd);
   printf("  PASS: test_build_exec_command_handles_special_chars\n");
}

static void test_build_exec_command_rejects_invalid(void)
{
   char *cmd = (char *)0x1; /* sentinel — must be NULLed on failure */
   assert(delegate_backend_ssh_build_exec_command(NULL, "h", "x", &cmd) == -1);
   assert(cmd == NULL);

   cmd = (char *)0x1;
   assert(delegate_backend_ssh_build_exec_command("/s", NULL, "x", &cmd) == -1);
   assert(cmd == NULL);

   cmd = (char *)0x1;
   assert(delegate_backend_ssh_build_exec_command("/s", "h", NULL, &cmd) == -1);
   assert(cmd == NULL);

   /* Empty ctrl_socket / host rejected — they would build a malformed
    * ssh invocation. */
   cmd = (char *)0x1;
   assert(delegate_backend_ssh_build_exec_command("", "h", "x", &cmd) == -1);
   assert(cmd == NULL);

   cmd = (char *)0x1;
   assert(delegate_backend_ssh_build_exec_command("/s", "", "x", &cmd) == -1);
   assert(cmd == NULL);

   /* NULL out pointer treated as invalid (would leak the buf). */
   assert(delegate_backend_ssh_build_exec_command("/s", "h", "x", NULL) == -1);
   printf("  PASS: test_build_exec_command_rejects_invalid\n");
}

static void test_socket_path_with_xdg_runtime_dir(void)
{
   setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
   char out[256] = {0};
   assert(delegate_backend_ssh_socket_path("task-A", out, sizeof(out)) == 0);
   assert(strcmp(out, "/run/user/1000/aimee-ssh-task-A.sock") == 0);
   printf("  PASS: test_socket_path_with_xdg_runtime_dir\n");
}

static void test_socket_path_without_xdg_uses_tmp_with_uid(void)
{
   unsetenv("XDG_RUNTIME_DIR");
   char out[256] = {0};
   assert(delegate_backend_ssh_socket_path("task-B", out, sizeof(out)) == 0);
   /* Path shape: /tmp/aimee-ssh-<uid>-task-B.sock */
   assert(strncmp(out, "/tmp/aimee-ssh-", 15) == 0);
   assert(strstr(out, "-task-B.sock") != NULL);
   printf("  PASS: test_socket_path_without_xdg_uses_tmp_with_uid\n");
}

static void test_socket_path_rejects_invalid(void)
{
   char out[256] = {0};
   assert(delegate_backend_ssh_socket_path(NULL, out, sizeof(out)) == -1);
   assert(delegate_backend_ssh_socket_path("", out, sizeof(out)) == -1);
   assert(delegate_backend_ssh_socket_path("task", NULL, 256) == -1);
   assert(delegate_backend_ssh_socket_path("task", out, 0) == -1);
   /* Buffer too small to hold the path. */
   char tiny[8];
   setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
   assert(delegate_backend_ssh_socket_path("very-long-task-id", tiny, sizeof(tiny)) == -1);
   unsetenv("XDG_RUNTIME_DIR");
   printf("  PASS: test_socket_path_rejects_invalid\n");
}

static void test_wait_for_socket_returns_immediately_when_present(void)
{
   /* Create the file first; the helper should return 0 on the very
    * first stat, no polling. */
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-test-wait-%d", (int)getpid());
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   assert(fd >= 0);
   close(fd);
   assert(delegate_backend_ssh_wait_for_socket(path, 1000) == 0);
   unlink(path);
   printf("  PASS: test_wait_for_socket_returns_immediately_when_present\n");
}

static void test_wait_for_socket_times_out_when_missing(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-test-missing-%d-%d", (int)getpid(), rand());
   /* 200ms budget; missing file → timeout. Helper polls every 50ms so
    * this resolves promptly. */
   assert(delegate_backend_ssh_wait_for_socket(path, 200) == -1);
   printf("  PASS: test_wait_for_socket_times_out_when_missing\n");
}

static void test_wait_for_socket_rejects_invalid_path(void)
{
   assert(delegate_backend_ssh_wait_for_socket(NULL, 100) == -1);
   assert(delegate_backend_ssh_wait_for_socket("", 100) == -1);
   printf("  PASS: test_wait_for_socket_rejects_invalid_path\n");
}

/* Set up an isolated workspace prefix under /tmp so SSH file ops can
 * mkdir/write/read without touching the real cwd. The fixture's exec
 * mode runs commands locally via bash; setting state->cwd causes
 * ssh_exec to prefix `cd /tmp/<prefix> &&` so the workspace_remote
 * relative path lands under /tmp/<prefix>/.aimee/delegate/<task>. */
static void setup_ssh_fileio_state(delegate_backend_t *b, const char *task_id, void **state_out)
{
   const char *fixture = write_fake_ssh_fixture(1);
   setenv("AIMEE_SSH_BIN", fixture, 1);
   delegate_backend_config_t cfg = {0};
   cfg.host = "h";
   assert(b->acquire(b, task_id, &cfg, state_out) == 0);
   /* Anchor the workspace under /tmp so we don't pollute cwd. */
   char anchor[256];
   snprintf(anchor, sizeof(anchor), "/tmp/aimee-ssh-fileio-%d", (int)getpid());
   mkdir(anchor, 0700);
   assert(b->set_cwd(b, *state_out, anchor) == 0);
}

static void teardown_ssh_fileio_state(delegate_backend_t *b, void *state)
{
   if (state)
      b->release(b, state, 0);
   /* Best-effort cleanup of the anchor dir. */
   char anchor[256];
   snprintf(anchor, sizeof(anchor), "/tmp/aimee-ssh-fileio-%d", (int)getpid());
   char rm[512];
   snprintf(rm, sizeof(rm), "rm -rf %s", anchor);
   (void)system(rm);
   unsetenv("AIMEE_SSH_BIN");
}

static void test_ssh_write_then_read_roundtrip(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   void *state = NULL;
   setup_ssh_fileio_state(b, "task-fileio-1", &state);

   assert(b->write_file(b, state, "hello.txt", "hello via ssh\n") == 0);
   char *content = NULL;
   assert(b->read_file(b, state, "hello.txt", 0, 0, &content) == 0);
   assert(content != NULL);
   assert(strcmp(content, "hello via ssh\n") == 0);
   free(content);

   teardown_ssh_fileio_state(b, state);
   printf("  PASS: test_ssh_write_then_read_roundtrip\n");
}

static void test_ssh_read_with_offset_and_limit(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   void *state = NULL;
   setup_ssh_fileio_state(b, "task-fileio-2", &state);

   assert(b->write_file(b, state, "data.txt", "0123456789abcdef") == 0);
   char *content = NULL;
   /* offset=4 limit=5 -> "45678" */
   assert(b->read_file(b, state, "data.txt", 4, 5, &content) == 0);
   assert(strcmp(content, "45678") == 0);
   free(content);

   teardown_ssh_fileio_state(b, state);
   printf("  PASS: test_ssh_read_with_offset_and_limit\n");
}

static void test_ssh_path_validation_rejects_escapes(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   void *state = NULL;
   setup_ssh_fileio_state(b, "task-fileio-3", &state);

   /* Absolute paths rejected. */
   assert(b->write_file(b, state, "/etc/passwd", "x") == -1);
   /* `..` segments rejected. */
   assert(b->write_file(b, state, "../escape.txt", "x") == -1);
   assert(b->write_file(b, state, "ok/../../escape", "x") == -1);
   char *content = (char *)0x1;
   assert(b->read_file(b, state, "../etc/passwd", 0, 0, &content) == -1);
   assert(content == NULL);

   teardown_ssh_fileio_state(b, state);
   printf("  PASS: test_ssh_path_validation_rejects_escapes\n");
}

static void test_ssh_list_dir_returns_entries(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_ssh();
   delegate_backend_t *b = delegate_backend_ssh_get();
   void *state = NULL;
   setup_ssh_fileio_state(b, "task-list-1", &state);

   /* Add a couple of files via write_file (which also creates parent
    * dirs); list_dir picks them up. */
   assert(b->write_file(b, state, "a.txt", "a") == 0);
   assert(b->write_file(b, state, "b.txt", "b") == 0);

   char **entries = NULL;
   int n = b->list_dir(b, state, ".", &entries);
   assert(n >= 2);
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

   teardown_ssh_fileio_state(b, state);
   printf("  PASS: test_ssh_list_dir_returns_entries\n");
}

int main(void)
{
   printf("delegate_backend_ssh:\n");
   test_register_puts_ssh_in_registry();
   test_file_ops_reject_null_state();
   test_build_exec_command_basic();
   test_build_exec_command_handles_special_chars();
   test_build_exec_command_rejects_invalid();
   test_socket_path_with_xdg_runtime_dir();
   test_socket_path_without_xdg_uses_tmp_with_uid();
   test_socket_path_rejects_invalid();
   test_wait_for_socket_returns_immediately_when_present();
   test_wait_for_socket_times_out_when_missing();
   test_wait_for_socket_rejects_invalid_path();
   test_acquire_succeeds_with_fake_ssh();
   test_acquire_fails_when_socket_never_appears();
   test_acquire_rejects_invalid_args();
   test_exec_runs_through_fake_ssh();
   test_exec_propagates_nonzero_exit();
   test_exec_set_cwd_prefixes_subsequent_calls();
   test_ssh_write_then_read_roundtrip();
   test_ssh_read_with_offset_and_limit();
   test_ssh_path_validation_rejects_escapes();
   test_ssh_list_dir_returns_entries();
   printf("ok\n");
   return 0;
}
