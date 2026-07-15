/* test_delegate_backend_docker.c: registry membership, pure helpers,
 * and fake-docker round trips for the docker backend. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "delegate_backend_docker.h"

/* Forward decls — definitions live further down with the rest of the
 * fixture-using cases; forward refs let us call them from earlier
 * tests. */
static const char *write_fake_docker_fixture(void);
static void teardown_fake_docker(void);
static int fake_container_exists(const char *container_name);

static void test_register_puts_docker_in_registry(void)
{
   delegate_backend_reset_for_test();
   assert(delegate_backend_register_docker() == 0);
   delegate_backend_t *b = delegate_backend_lookup("docker");
   assert(b != NULL);
   assert(b == delegate_backend_docker_get());
   /* Idempotent — second register call rejected by the registry. */
   assert(delegate_backend_register_docker() == -1);
   /* All vtable slots wired (no NULL pointers). */
   assert(b->acquire && b->release && b->exec);
   assert(b->read_file && b->write_file && b->list_dir);
   assert(b->get_cwd && b->set_cwd);
   printf("  PASS: test_register_puts_docker_in_registry\n");
}

static void test_file_ops_reject_null_state(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   char *out = (char *)0x1;
   assert(b->read_file(b, NULL, "x", 0, 0, &out) == -1);
   assert(out == NULL);
   assert(b->write_file(b, NULL, "p", "c") == -1);
   char **entries = (char **)0x1;
   assert(b->list_dir(b, NULL, ".", &entries) == -1);
   assert(entries == NULL);
   printf("  PASS: test_file_ops_reject_null_state\n");
}

/* Override AIMEE_DOCKER_WORKDIR so file ops resolve under a writable
 * /tmp anchor instead of the real /workspace path (which would need
 * root). Combined with the fake-docker fixture's exec mode that runs
 * commands locally, this lets file-op tests round-trip through bash
 * without a real container. */
static void setup_docker_fileio_state(delegate_backend_t *b, const char *task_id, void **state_out)
{
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);
   /* Anchor the in-container WORKDIR under /tmp for the duration of
    * the test. mkdir is idempotent; the per-pid suffix avoids cross-
    * test pollution. */
   char workdir[256];
   snprintf(workdir, sizeof(workdir), "/tmp/aimee-docker-workdir-%d", (int)getpid());
   mkdir(workdir, 0700);
   setenv("AIMEE_DOCKER_WORKDIR", workdir, 1);

   delegate_backend_config_t cfg = {0};
   assert(b->acquire(b, task_id, &cfg, state_out) == 0);
}

static void teardown_docker_fileio_state(delegate_backend_t *b, void *state)
{
   if (state)
      b->release(b, state, 0);
   teardown_fake_docker();
   char rm[512];
   snprintf(rm, sizeof(rm), "rm -rf /tmp/aimee-docker-workdir-%d", (int)getpid());
   (void)system(rm);
   unsetenv("AIMEE_DOCKER_BIN");
   unsetenv("AIMEE_DOCKER_WORKDIR");
}

static void test_docker_write_then_read_roundtrip(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   void *state = NULL;
   setup_docker_fileio_state(b, "task-fileio-1", &state);

   assert(b->write_file(b, state, "hello.txt", "hello in container\n") == 0);
   char *content = NULL;
   assert(b->read_file(b, state, "hello.txt", 0, 0, &content) == 0);
   assert(content != NULL);
   assert(strcmp(content, "hello in container\n") == 0);
   free(content);

   teardown_docker_fileio_state(b, state);
   printf("  PASS: test_docker_write_then_read_roundtrip\n");
}

static void test_docker_path_validation_rejects_escapes(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   void *state = NULL;
   setup_docker_fileio_state(b, "task-fileio-2", &state);

   assert(b->write_file(b, state, "/etc/passwd", "x") == -1);
   assert(b->write_file(b, state, "../escape.txt", "x") == -1);
   assert(b->write_file(b, state, "ok/../../escape", "x") == -1);
   char *content = (char *)0x1;
   assert(b->read_file(b, state, "../etc/passwd", 0, 0, &content) == -1);
   assert(content == NULL);

   teardown_docker_fileio_state(b, state);
   printf("  PASS: test_docker_path_validation_rejects_escapes\n");
}

static void test_docker_list_dir_returns_entries(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   void *state = NULL;
   setup_docker_fileio_state(b, "task-list-1", &state);

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

   teardown_docker_fileio_state(b, state);
   printf("  PASS: test_docker_list_dir_returns_entries\n");
}

/* Write a fake-docker fixture script. Honors:
 *   docker start <name>           -> exit 0 if .exists flag present, else 1
 *   docker create --name N ...    -> touch .exists flag, exit 0
 *   docker stop <name>            -> exit 0 (we don't track running state)
 *   docker rm -f <name>           -> remove the .exists flag
 *   docker exec -i N bash -c CMD  -> exec bash -c "CMD" locally so the
 *                                    test exercises the full exec path
 *                                    without a real docker daemon
 * State files live under /tmp/aimee-fake-docker-state-<pid>/. */
static const char *write_fake_docker_fixture(void)
{
   static char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-fake-docker-%d.sh", (int)getpid());
   char state_dir[256];
   snprintf(state_dir, sizeof(state_dir), "/tmp/aimee-fake-docker-state-%d", (int)getpid());
   mkdir(state_dir, 0700);

   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f,
           "#!/bin/bash\n"
           "STATE_DIR=%s\n"
           "case \"$1\" in\n"
           "  start)\n"
           "    name=\"$2\"\n"
           "    [ -f \"$STATE_DIR/$name.exists\" ] && exit 0\n"
           "    exit 1\n"
           "    ;;\n"
           "  create)\n"
           "    printf '%%s\\n' \"$@\" > \"$STATE_DIR/create.argv\"\n"
           "    shift\n"
           "    name=\"\"\n"
           "    while [ $# -gt 0 ]; do\n"
           "      if [ \"$1\" = \"--name\" ]; then name=\"$2\"; shift 2; continue; fi\n"
           "      shift\n"
           "    done\n"
           "    [ -n \"$name\" ] && touch \"$STATE_DIR/$name.exists\"\n"
           "    exit 0\n"
           "    ;;\n"
           "  stop)\n"
           "    exit 0\n"
           "    ;;\n"
           "  rm)\n"
           "    name=\"${@: -1}\"\n"
           "    rm -f \"$STATE_DIR/$name.exists\"\n"
           "    exit 0\n"
           "    ;;\n"
           "  exec)\n"
           "    # docker exec -i <name> bash -c <cmd>; LAST argv is the\n"
           "    # b64-wrapped command. Run it locally via bash.\n"
           "    cmd=\"${@: -1}\"\n"
           "    exec bash -c \"$cmd\"\n"
           "    ;;\n"
           "  *)\n"
           "    exit 99\n"
           "    ;;\n"
           "esac\n",
           state_dir);
   fclose(f);
   chmod(path, 0700);
   return path;
}

static void teardown_fake_docker(void)
{
   char rm[256];
   snprintf(rm, sizeof(rm), "rm -rf /tmp/aimee-fake-docker-state-%d /tmp/aimee-fake-docker-%d.sh",
            (int)getpid(), (int)getpid());
   (void)system(rm);
}

static int fake_container_exists(const char *container_name)
{
   char p[512];
   snprintf(p, sizeof(p), "/tmp/aimee-fake-docker-state-%d/%s.exists", (int)getpid(),
            container_name);
   struct stat s;
   return stat(p, &s) == 0;
}

static void test_acquire_creates_and_starts_container(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   cfg.image = "ubuntu:22.04";
   void *state = NULL;
   assert(b->acquire(b, "task-acq-1", &cfg, &state) == 0);
   assert(state != NULL);
   /* The fixture's "create" handler should have touched the .exists
    * flag for the canonical container name. */
   assert(fake_container_exists("aimee-delegate-task-acq-1"));

   /* release(hibernate=0) → docker rm -f → flag removed. */
   b->release(b, state, 0);
   assert(!fake_container_exists("aimee-delegate-task-acq-1"));

   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_acquire_creates_and_starts_container\n");
}

static void test_release_hibernate_keeps_container(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-hib-1", &cfg, &state) == 0);
   assert(fake_container_exists("aimee-delegate-task-hib-1"));

   /* hibernate=1 → docker stop, container persists. */
   b->release(b, state, 1);
   assert(fake_container_exists("aimee-delegate-task-hib-1"));

   /* Re-acquire → docker start (fixture sees the .exists flag) →
    * resumes the same container, no second create. */
   void *state2 = NULL;
   assert(b->acquire(b, "task-hib-1", &cfg, &state2) == 0);
   b->release(b, state2, 0);
   assert(!fake_container_exists("aimee-delegate-task-hib-1"));

   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_release_hibernate_keeps_container\n");
}

static void test_docker_exec_runs_through_fake(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-exec-1", &cfg, &state) == 0);

   char out[4096] = {0}, err[4096] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "echo hello-from-docker", 5000, &r) == 0);
   assert(r.exit_code == 0);
   assert(strcmp(out, "hello-from-docker\n") == 0);
   b->release(b, state, 0);

   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_docker_exec_runs_through_fake\n");
}

static void test_docker_exec_propagates_nonzero_exit(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-exec-2", &cfg, &state) == 0);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "exit 9", 5000, &r) == 0);
   assert(r.exit_code == 9);
   b->release(b, state, 0);

   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_docker_exec_propagates_nonzero_exit\n");
}

static void test_docker_exec_set_cwd_prefixes_subsequent_calls(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-exec-cwd", &cfg, &state) == 0);

   /* Default get_cwd reflects the container's WORKDIR (or the
    * AIMEE_DOCKER_WORKDIR override if set; this test leaves it
    * unset so it lands on the production default). */
   char *cwd = NULL;
   assert(b->get_cwd(b, state, &cwd) == 0);
   assert(strcmp(cwd, "/workspace") == 0);
   free(cwd);

   /* set_cwd to /tmp and exec pwd → "/tmp\n". */
   assert(b->set_cwd(b, state, "/tmp") == 0);
   char out[512] = {0}, err[512] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, "pwd", 5000, &r) == 0);
   assert(strcmp(out, "/tmp\n") == 0);

   b->release(b, state, 0);
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_docker_exec_set_cwd_prefixes_subsequent_calls\n");
}

static void test_acquire_rejects_invalid_args(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   /* Empty task_id rejected. */
   void *state = (void *)0x1;
   assert(b->acquire(b, "", NULL, &state) == -1);
   assert(state == NULL);
   /* NULL state_out rejected. */
   assert(b->acquire(b, "task", NULL, NULL) == -1);
   printf("  PASS: test_acquire_rejects_invalid_args\n");
}

static void test_container_name_basic(void)
{
   char name[128] = {0};
   assert(delegate_backend_docker_container_name("task-abc-123", name, sizeof(name)) == 0);
   assert(strcmp(name, "aimee-delegate-task-abc-123") == 0);
   printf("  PASS: test_container_name_basic\n");
}

static void test_container_name_sanitises_invalid_chars(void)
{
   /* Spaces, slashes, colons all replaced with '_'; alnum + _.- kept. */
   char name[128] = {0};
   assert(delegate_backend_docker_container_name("foo bar:baz/qux.v1-2", name, sizeof(name)) == 0);
   assert(strcmp(name, "aimee-delegate-foo_bar_baz_qux.v1-2") == 0);
   printf("  PASS: test_container_name_sanitises_invalid_chars\n");
}

static void test_container_name_rejects_invalid(void)
{
   char name[128] = {0};
   assert(delegate_backend_docker_container_name(NULL, name, sizeof(name)) == -1);
   assert(delegate_backend_docker_container_name("", name, sizeof(name)) == -1);
   assert(delegate_backend_docker_container_name("task", NULL, 128) == -1);
   assert(delegate_backend_docker_container_name("task", name, 0) == -1);
   /* Buffer too small. */
   char tiny[8];
   assert(delegate_backend_docker_container_name("very-long-task", tiny, sizeof(tiny)) == -1);
   printf("  PASS: test_container_name_rejects_invalid\n");
}

static void test_build_exec_command_basic(void)
{
   char *cmd = NULL;
   assert(delegate_backend_docker_build_exec_command("aimee-delegate-task1", "echo hello", &cmd) ==
          0);
   assert(cmd != NULL);
   assert(strstr(cmd, "docker exec -i aimee-delegate-task1 bash -c") != NULL);
   assert(strstr(cmd, "base64 -d | bash") != NULL);
   /* Raw user command b64-encoded — must NOT appear directly. */
   assert(strstr(cmd, "echo hello") == NULL);
   free(cmd);
   printf("  PASS: test_build_exec_command_basic\n");
}

static void test_build_exec_command_handles_special_chars(void)
{
   const char *evil = "echo 'with quotes' && echo `backticks` && echo $HOME";
   char *cmd = NULL;
   assert(delegate_backend_docker_build_exec_command("c", evil, &cmd) == 0);
   /* Raw evil string MUST NOT appear; the b64 envelope hides it. */
   assert(strstr(cmd, "with quotes") == NULL);
   assert(strstr(cmd, "backticks") == NULL);
   free(cmd);
   printf("  PASS: test_build_exec_command_handles_special_chars\n");
}

static void test_build_exec_command_rejects_invalid(void)
{
   char *cmd = (char *)0x1;
   assert(delegate_backend_docker_build_exec_command(NULL, "x", &cmd) == -1);
   assert(cmd == NULL);
   cmd = (char *)0x1;
   assert(delegate_backend_docker_build_exec_command("c", NULL, &cmd) == -1);
   assert(cmd == NULL);
   cmd = (char *)0x1;
   assert(delegate_backend_docker_build_exec_command("", "x", &cmd) == -1);
   assert(cmd == NULL);
   assert(delegate_backend_docker_build_exec_command("c", "x", NULL) == -1);
   printf("  PASS: test_build_exec_command_rejects_invalid\n");
}

/* The create argv the fake docker was invoked with (NULL if it never ran).
 * Needed to assert WHAT gets mounted: the caller's tree, or a scratch dir. */
static char *read_fake_docker_create_argv(void)
{
   char path[256];
   snprintf(path, sizeof(path), "/tmp/aimee-fake-docker-state-%d/create.argv", (int)getpid());
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   char *buf = calloc(1, 8192);
   if (buf)
      (void)!fread(buf, 1, 8191, f);
   fclose(f);
   return buf;
}

/* cfg.workspace: mount the caller's tree AS the workspace.
 *
 * Without it the backend mints an empty scratch dir under $XDG_CACHE_HOME and
 * mounts THAT — which is exactly why a delegate could not read its own subject:
 * it opens the file named in its task and finds nothing, then reasons about code
 * it cannot see. This is the difference between a sandbox and a blindfold. */
static void test_docker_mounts_caller_workspace(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   /* A real git checkout: the backend refuses to bind-mount anything that is not
    * one, because the caller derives the path from a session cwd. */
   char tree[256];
   snprintf(tree, sizeof(tree), "/tmp/aimee-ws-tree-%d", (int)getpid());
   mkdir(tree, 0700);
   char gitdir[300];
   snprintf(gitdir, sizeof(gitdir), "%s/.git", tree);
   mkdir(gitdir, 0700);

   delegate_backend_config_t cfg = {0};
   cfg.workspace = tree;
   void *state = NULL;
   assert(b->acquire(b, "task-ws-1", &cfg, &state) == 0);
   assert(state != NULL);
   /* The docker create argv must carry `-v <tree>:<workdir>` — the tree itself,
    * not a scratch dir. The fixture records the argv it was called with. */
   char *log = read_fake_docker_create_argv();
   assert(log != NULL);
   assert(strstr(log, tree) != NULL);
   /* Must run as the server's uid:gid: root-owned files in the user's checkout
    * would be unremovable by them and make git refuse the tree entirely. */
   assert(strstr(log, "--user") != NULL);
   char uidgid[64];
   snprintf(uidgid, sizeof(uidgid), "%u:%u", (unsigned)getuid(), (unsigned)getgid());
   assert(strstr(log, uidgid) != NULL);
   free(log);
   b->release(b, state, 0);

   /* read-only mode must reach the docker argv as :ro — the mount is where the
    * isolation is enforced, not the guard above it. */
   delegate_backend_config_t rocfg = {0};
   rocfg.workspace = tree;
   rocfg.workspace_read_only = 1;
   void *rostate = NULL;
   assert(b->acquire(b, "task-ws-ro", &rocfg, &rostate) == 0);
   char *rolog = read_fake_docker_create_argv();
   assert(rolog != NULL);
   assert(strstr(rolog, ":ro") != NULL);
   free(rolog);
   b->release(b, rostate, 0);

   char rm[512];
   snprintf(rm, sizeof(rm), "rm -rf %s", tree);
   (void)system(rm);
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: docker_mounts_caller_workspace\n");
}

/* A workspace path that does not exist must be REFUSED, not created.
 *
 * The scratch path is ours to mkdir; a caller naming a directory is naming
 * something it already has. Creating it on their behalf turns a typo into an
 * empty workspace that looks like it worked — and an empty tree reads to a
 * delegate as "the code is missing", which is a far worse lie than an error. */
static void test_docker_refuses_a_missing_workspace(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   char missing[256];
   snprintf(missing, sizeof(missing), "/tmp/aimee-ws-does-not-exist-%d", (int)getpid());
   delegate_backend_config_t cfg = {0};
   cfg.workspace = missing;
   void *state = NULL;
   assert(b->acquire(b, "task-ws-2", &cfg, &state) == -1);
   assert(state == NULL);
   /* And it must not have created it as a side effect. */
   struct stat st;
   assert(stat(missing, &st) != 0);

   /* A regular file is not a tree either. */
   char afile[256];
   snprintf(afile, sizeof(afile), "/tmp/aimee-ws-file-%d", (int)getpid());
   FILE *f = fopen(afile, "w");
   assert(f != NULL);
   fputs("x", f);
   fclose(f);
   cfg.workspace = afile;
   assert(b->acquire(b, "task-ws-3", &cfg, &state) == -1);
   unlink(afile);

   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: docker_refuses_a_missing_workspace\n");
}

/* Each of these is a way a delegate could end up reasoning about a tree that is
 * not the one it was told about — the panel found every one of them. */
static void test_docker_workspace_validation_refusals(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);
   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   char base[256];
   snprintf(base, sizeof(base), "/tmp/aimee-ws-val-%d", (int)getpid());
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", base, base);
   (void)system(cmd);

   /* Not a git checkout: the caller derives this from a session cwd, so without
    * the check an unlucky cwd ('/', a secrets dir, the server's own tree) becomes
    * a read-write bind mount into a delegate container. */
   char plain[300];
   snprintf(plain, sizeof(plain), "%s/plain", base);
   mkdir(plain, 0700);
   cfg.workspace = plain;
   assert(b->acquire(b, "task-val-1", &cfg, &state) == -1);
   assert(state == NULL);

   /* A LINKED worktree is now MOUNTED, not refused: the repo goes in read-only at
    * its own absolute path so the gitlink resolves verbatim, with the worktree and
    * its gitdir nested writable over it. Validated on docker 26.1.5: git status
    * refreshes its index in the per-worktree gitdir, git describe works, and a
    * write outside the worktree fails with "Read-only file system". */
   char repo[300];
   snprintf(repo, sizeof(repo), "%s/repo", base);
   char repogit[400];
   snprintf(repogit, sizeof(repogit), "%s/.git", repo);
   char wtdir[500];
   snprintf(wtdir, sizeof(wtdir), "%s/worktrees/task01", repogit);
   char cmd2[900];
   snprintf(cmd2, sizeof(cmd2), "mkdir -p %s %s/.aimee/worktrees/k/task01", wtdir, repo);
   (void)system(cmd2);
   char wt2[400];
   snprintf(wt2, sizeof(wt2), "%s/.aimee/worktrees/k/task01", repo);
   char gitfile2[500];
   snprintf(gitfile2, sizeof(gitfile2), "%s/.git", wt2);
   FILE *g2 = fopen(gitfile2, "w");
   assert(g2 != NULL);
   fprintf(g2, "gitdir: %s\n", wtdir);
   fclose(g2);

   cfg.workspace = wt2;
   cfg.workspace_read_only = 0;
   assert(b->acquire(b, "task-wt", &cfg, &state) == 0);
   {
      char *l = read_fake_docker_create_argv();
      assert(l != NULL);
      /* the repo mounted READ-ONLY at its own path — the gitlink target */
      char m_repo[700];
      snprintf(m_repo, sizeof(m_repo), "%s:%s:ro", repo, repo);
      assert(strstr(l, m_repo) != NULL);
      /* the worktree writable, nested over it */
      char m_wt[900];
      snprintf(m_wt, sizeof(m_wt), "%s:%s", wt2, wt2);
      assert(strstr(l, m_wt) != NULL);
      /* the per-worktree gitdir writable — this is where git status writes its index */
      char m_gd[1100];
      snprintf(m_gd, sizeof(m_gd), "%s:%s", wtdir, wtdir);
      assert(strstr(l, m_gd) != NULL);
      /* and the workdir is the worktree, not /workspace */
      assert(strstr(l, wt2) != NULL);
      free(l);
   }
   b->release(b, state, 0);

   /* A worktree whose .git carries no absolute gitdir cannot be mounted usefully:
    * refuse rather than leave git broken inside the container. */
   char wtbad[400];
   snprintf(wtbad, sizeof(wtbad), "%s/wtbad", base);
   mkdir(wtbad, 0700);
   char gbad[500];
   snprintf(gbad, sizeof(gbad), "%s/.git", wtbad);
   FILE *fb = fopen(gbad, "w");
   assert(fb != NULL);
   fputs("gitdir: ../relative/path\n", fb);
   fclose(fb);
   cfg.workspace = wtbad;
   assert(b->acquire(b, "task-wtbad", &cfg, &state) == -1);

   /* A symlink to a valid checkout must not slip past canonicalization: stat()
    * follows symlinks and so does the daemon, so the mount could land somewhere
    * the checks never saw. Canonicalized, this one IS a real checkout, so it is
    * accepted — and the argv must carry the RESOLVED path, not the link. */
   char realrepo[300], link[300];
   snprintf(realrepo, sizeof(realrepo), "%s/realrepo", base);
   mkdir(realrepo, 0700);
   char rg[400];
   snprintf(rg, sizeof(rg), "%s/.git", realrepo);
   mkdir(rg, 0700);
   snprintf(link, sizeof(link), "%s/link", base);
   assert(symlink(realrepo, link) == 0);
   cfg.workspace = link;
   assert(b->acquire(b, "task-val-3", &cfg, &state) == 0);
   char *log = read_fake_docker_create_argv();
   assert(log != NULL);
   assert(strstr(log, realrepo) != NULL); /* the resolved path */
   free(log);
   b->release(b, state, 0);

   snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
   (void)system(cmd);
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: docker_workspace_validation_refusals\n");
}

int main(void)
{
   printf("delegate_backend_docker:\n");
   test_register_puts_docker_in_registry();
   test_file_ops_reject_null_state();
   test_container_name_basic();
   test_container_name_sanitises_invalid_chars();
   test_container_name_rejects_invalid();
   test_build_exec_command_basic();
   test_build_exec_command_handles_special_chars();
   test_build_exec_command_rejects_invalid();
   test_acquire_creates_and_starts_container();
   test_release_hibernate_keeps_container();
   test_docker_exec_runs_through_fake();
   test_docker_exec_propagates_nonzero_exit();
   test_docker_exec_set_cwd_prefixes_subsequent_calls();
   test_acquire_rejects_invalid_args();
   test_docker_write_then_read_roundtrip();
   test_docker_path_validation_rejects_escapes();
   test_docker_list_dir_returns_entries();
   test_docker_mounts_caller_workspace();
   test_docker_refuses_a_missing_workspace();
   test_docker_workspace_validation_refusals();
   printf("ok\n");
   return 0;
}
