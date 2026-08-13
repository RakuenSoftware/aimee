/* test_delegate_backend_docker.c: registry membership, pure helpers,
 * and fake-docker round trips for the docker backend. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "aimee_home.h"
#include <aimee/delegates/delegate_backend_docker.h>
#include <aimee/delegates/delegate_launch_args.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* The isolation verdict, faked.
 *
 * These cases are about the backend's create/start/resume behaviour, not about
 * what a network report means -- that is judged against the module. The fake
 * records what it was asked and answers "isolated" so the flow proceeds; a case
 * that needs a refusal sets g_fake_isolation_refuse. */
static int g_fake_isolation_refuse;
static int g_fake_isolation_calls;
static char g_fake_isolation_report[4096];
static int g_fake_isolation_probe_failed;

static int fake_isolation(const char *report, int probe_failed, int require_isolation, int *refuse,
                          int *warn, int *is_error, char *reason, size_t reason_cap)
{
   (void)require_isolation;
   g_fake_isolation_calls++;
   snprintf(g_fake_isolation_report, sizeof(g_fake_isolation_report), "%s", report ? report : "");
   g_fake_isolation_probe_failed = probe_failed;
   *refuse = g_fake_isolation_refuse;
   *warn = 0;
   *is_error = g_fake_isolation_refuse;
   if (reason && reason_cap)
      snprintf(reason, reason_cap, "%s", g_fake_isolation_refuse ? "fake breach" : "");
   return 0;
}

/* ── the module's answer, faked ─────────────────────────────────────────────
 *
 * The backend no longer decides the container's shape. What it still does, and
 * what these tests are now for, is DISCOVERY: resolving the workspace, reading
 * the gitlink, deriving the repo root, verifying the backlink, and handing all
 * of that over as a spec.
 *
 * So this provider records the spec it was given -- that is the backend's
 * actual output -- and returns a minimal argv so the create/start/resume flow
 * still runs. What goes INTO the argv (--network none, the mount layering, the
 * environment, the container name) is the module's rule and is tested against
 * the module, not through a fake of it. */
static aimee_delegates_launch_spec_t g_last_launch;
static char g_last_repo[4096], g_last_worktree[4096];
static char g_last_gitdir[4096], g_last_scratch[4096];
static char g_last_socket_host[4096], g_last_user[64], g_last_table[4096];
static int g_launch_calls;

static void copy_field(char *dst, size_t cap, const char *src)
{
   snprintf(dst, cap, "%s", src ? src : "");
}

static int fake_launch_args(const aimee_delegates_launch_spec_t *spec, char *name_out,
                            size_t name_cap, const char **argv_out, size_t argv_cap,
                            size_t *arg_len_out, uint8_t *buf, size_t buf_cap)
{
   g_launch_calls++;
   g_last_launch = *spec;
   copy_field(g_last_repo, sizeof(g_last_repo), spec->repo_root);
   copy_field(g_last_worktree, sizeof(g_last_worktree), spec->worktree);
   copy_field(g_last_gitdir, sizeof(g_last_gitdir), spec->gitdir);
   copy_field(g_last_scratch, sizeof(g_last_scratch), spec->scratch_dir);
   copy_field(g_last_socket_host, sizeof(g_last_socket_host), spec->parent_socket_host);
   copy_field(g_last_user, sizeof(g_last_user), spec->run_as_user);
   copy_field(g_last_table, sizeof(g_last_table), spec->mount_table);

   /* The name the module would return: prefix + task id, as ContainerName does
    * for a task with no mounts. The tests only need it to be stable. */
   snprintf(name_out, name_cap, "aimee-delegate-%s", spec->task_id ? spec->task_id : "");

   /* A minimal but real create command, written into the caller's buffer the
    * way a decoded response would be. */
   const char *args[] = {"create",    "--name", name_out,
                         "--network", "none",   spec->image ? spec->image : "ubuntu:22.04"};
   size_t argc = sizeof(args) / sizeof(args[0]);
   if (argc > argv_cap)
      return -1;
   size_t at = 0;
   for (size_t i = 0; i < argc; i++)
   {
      size_t n = strlen(args[i]);
      if (at + n + 1 > buf_cap)
         return -1;
      memcpy(buf + at, args[i], n);
      argv_out[i] = (const char *)(buf + at);
      arg_len_out[i] = n;
      at += n + 1;
   }
   return (int)argc;
}

/* Forward decls — definitions live further down with the rest of the
 * fixture-using cases; forward refs let us call them from earlier
 * tests. */
static const char *write_fake_docker_fixture(void);
static void teardown_fake_docker(void);
static int fake_container_exists(const char *container_name);

static void test_translate_named_volume_socket_path(void)
{
   char out[512];
   const char *mounts = "/var/lib/aimee\t/var/lib/docker/volumes/aimee_home/_data\n"
                        "/var/lib/aimee-workspaces\t/srv/aimee/workspaces\n";
   assert(delegate_backend_docker_translate_mount_path("/var/lib/aimee/aimee-http.sock", mounts,
                                                       out, sizeof(out)) == 1);
   assert(strcmp(out, "/var/lib/docker/volumes/aimee_home/_data/aimee-http.sock") == 0);
   assert(delegate_backend_docker_translate_mount_path("/opt/aimee/aimee-http.sock", mounts, out,
                                                       sizeof(out)) == 0);
   assert(strcmp(out, "/opt/aimee/aimee-http.sock") == 0);
   assert(delegate_backend_docker_translate_mount_path("/var/lib/aimee-other/socket", mounts, out,
                                                       sizeof(out)) == 0);
   assert(strcmp(out, "/var/lib/aimee-other/socket") == 0);
   assert(delegate_backend_docker_translate_mount_path("/opt/aimee/socket", "/\t/srv/root\n", out,
                                                       sizeof(out)) == 1);
   assert(strcmp(out, "/srv/root/opt/aimee/socket") == 0);
   printf("  PASS: test_translate_named_volume_socket_path\n");
}

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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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

/* A file read must return the file, and nothing after it.
 *
 * docker_exec writes the captured output into a caller-allocated buffer and
 * docker_read_file measures it with strlen(). If the terminator goes at the END
 * OF THE BUFFER rather than the end of the output, everything between is
 * whatever the allocator last left there -- so strlen() walks past the file into
 * heap garbage and the delegate is handed its own file with plausible-looking
 * bytes appended.
 *
 * That read correctly for as long as the buffers were fresh zeroed pages, which
 * is why it survived: the bug needs a dirty arena to show. This dirties one on
 * purpose, the way any real allocation before the read would. */
static void test_read_file_returns_only_the_file(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_backend_t *b = delegate_backend_docker_get();
   void *state = NULL;
   setup_docker_fileio_state(b, "task-dirty-1", &state);

   /* Dirty the arena with a recognisable pattern and release it, so the read
    * buffer below is very likely to be handed the same memory. */
   for (int i = 0; i < 4; i++)
   {
      size_t n = 1u << 18;
      char *scratch = malloc(n);
      assert(scratch != NULL);
      memset(scratch, 'X', n - 1);
      scratch[n - 1] = '\0';
      free(scratch);
   }

   assert(b->write_file(b, state, "exact.txt", "nine char") == 0);
   char *content = NULL;
   assert(b->read_file(b, state, "exact.txt", 0, 0, &content) == 0);
   assert(content != NULL);
   assert(strlen(content) == 9);
   assert(strcmp(content, "nine char") == 0);
   free(content);

   /* An EMPTY file must come back empty, not as whatever was in the buffer. */
   assert(b->write_file(b, state, "empty.txt", "") == 0);
   content = NULL;
   assert(b->read_file(b, state, "empty.txt", 0, 0, &content) == 0);
   assert(content != NULL && content[0] == '\0');
   free(content);

   teardown_docker_fileio_state(b, state);
   printf("  PASS: test_read_file_returns_only_the_file\n");
}

/* The native file tools (tool_read_file/tool_write_file) resolve to an ABSOLUTE
 * in-workspace path via the thread cwd before calling the provider. The worktree is
 * bind-mounted path-identically, so that path is valid in-container and MUST be accepted
 * — previously it was rejected outright ("cannot open"/"cannot write" on a container
 * delegate's own worktree), while a path OUTSIDE the workspace must still be refused. */
static void test_docker_absolute_in_workspace_path_accepted(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
   delegate_backend_t *b = delegate_backend_docker_get();
   void *state = NULL;
   setup_docker_fileio_state(b, "task-abs-1", &state);

   char workdir[256];
   snprintf(workdir, sizeof(workdir), "/tmp/aimee-docker-workdir-%d", (int)getpid());
   char abs[512];

   /* Read back a relative-written file via its ABSOLUTE in-workspace path. */
   assert(b->write_file(b, state, "note.txt", "abs read\n") == 0);
   snprintf(abs, sizeof(abs), "%s/note.txt", workdir);
   char *content = NULL;
   assert(b->read_file(b, state, abs, 0, 0, &content) == 0);
   assert(content != NULL && strcmp(content, "abs read\n") == 0);
   free(content);

   /* Write via an ABSOLUTE in-workspace path, then read it back relatively. */
   snprintf(abs, sizeof(abs), "%s/note2.txt", workdir);
   assert(b->write_file(b, state, abs, "abs write\n") == 0);
   content = NULL;
   assert(b->read_file(b, state, "note2.txt", 0, 0, &content) == 0);
   assert(content != NULL && strcmp(content, "abs write\n") == 0);
   free(content);

   /* An absolute path OUTSIDE the workspace root is still refused (no host escape),
    * including a sibling that merely shares the workdir prefix. */
   assert(b->write_file(b, state, "/tmp/outside-escape.txt", "x") == -1);
   char sibling[320];
   snprintf(sibling, sizeof(sibling), "%s-evil/x.txt", workdir);
   assert(b->write_file(b, state, sibling, "x") == -1);

   teardown_docker_fileio_state(b, state);
   printf("  PASS: test_docker_absolute_in_workspace_path_accepted\n");
}

static void test_docker_path_validation_rejects_escapes(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
 *   docker ps -aq --filter ...    -> print names of .exists state files
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
           "    count=0; [ -f \"$STATE_DIR/create.count\" ] && read -r count < "
           "\"$STATE_DIR/create.count\"\n"
           "    printf '%%s\\n' \"$((count + 1))\" > \"$STATE_DIR/create.count\"\n"
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
           "  ps)\n"
           "    for path in \"$STATE_DIR\"/*.exists; do\n"
           "      [ -e \"$path\" ] || continue\n"
           "      name=\"${path##*/}\"\n"
           "      id=\"${name%%.exists}\"\n"
           "      printf '%%s aimee-delegate-%%s\\n' \"$id\" \"$id\"\n"
           "    done\n"
           "    exit 0\n"
           "    ;;\n"
           "  rm)\n"
           "    name=\"${@: -1}\"\n"
           "    rm -f \"$STATE_DIR/$name.exists\"\n"
           "    exit 0\n"
           "    ;;\n"
           "  inspect)\n"
           "    name=\"${@: -1}\"\n"
           "    if [[ \"$name\" = aimee-delegate-* ]] && "
           "[ -n \"$AIMEE_FAKE_DOCKER_SANDBOX_MOUNTS\" ]; then\n"
           "      printf '%%b' \"$AIMEE_FAKE_DOCKER_SANDBOX_MOUNTS\"\n"
           "    elif [ -n \"$AIMEE_FAKE_DOCKER_INSPECT_MOUNTS\" ]; then\n"
           "      printf '%%b' \"$AIMEE_FAKE_DOCKER_INSPECT_MOUNTS\"\n"
           "    else\n"
           "      printf 'none=;\\n'\n"
           "    fi\n"
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

static void test_remove_orphans_accepts_only_container_ids(void)
{
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   char valid_path[512], valid_long_path[512], invalid_path[512];
   snprintf(valid_path, sizeof(valid_path), "/tmp/aimee-fake-docker-state-%d/0123456789ab.exists",
            (int)getpid());
   snprintf(valid_long_path, sizeof(valid_long_path),
            "/tmp/aimee-fake-docker-state-%d/"
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.exists",
            (int)getpid());
   snprintf(invalid_path, sizeof(invalid_path),
            "/tmp/aimee-fake-docker-state-%d/"
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0.exists",
            (int)getpid());
   FILE *f = fopen(valid_path, "w");
   assert(f != NULL);
   fclose(f);
   f = fopen(valid_long_path, "w");
   assert(f != NULL);
   fclose(f);
   f = fopen(invalid_path, "w");
   assert(f != NULL);
   fclose(f);

   assert(delegate_backend_docker_remove_orphans() == 2);
   assert(!fake_container_exists("0123456789ab"));
   assert(
       !fake_container_exists("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
   assert(
       fake_container_exists("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"));

   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_remove_orphans_accepts_only_container_ids\n");
}

static int fake_create_count(void)
{
   char p[512];
   snprintf(p, sizeof(p), "/tmp/aimee-fake-docker-state-%d/create.count", (int)getpid());
   FILE *f = fopen(p, "r");
   if (!f)
      return 0;
   int count = 0;
   (void)fscanf(f, "%d", &count);
   fclose(f);
   return count;
}

/* The backend's job with an isolation verdict is to honour it: destroy the
 * container and refuse the delegation. What the report MEANT is judged in the
 * module and tested there. */
static void test_acquire_refuses_when_isolation_is_refused(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   g_fake_isolation_refuse = 1;
   g_fake_isolation_calls = 0;
   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-iso-refuse", &cfg, &state) == DELEGATE_ACQUIRE_REFUSED_ISOLATION);
   assert(state == NULL);
   assert(g_fake_isolation_calls == 1);
   /* The container must not be left running for something else to reuse. */
   assert(!fake_container_exists("aimee-delegate-task-iso-refuse"));

   g_fake_isolation_refuse = 0;
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_acquire_refuses_when_isolation_is_refused\n");
}

/* No verdict is not a pass. A sandbox nobody could assess is not an assessed
 * sandbox, so an unanswered judgement refuses exactly as a failed probe would
 * under require_isolation -- otherwise removing the provider would silently
 * turn every container into an unchecked one. */
static void test_acquire_refuses_when_isolation_cannot_be_judged(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(NULL); /* nothing to judge with */
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-iso-unjudged", &cfg, &state) == DELEGATE_ACQUIRE_REFUSED_ISOLATION);
   assert(state == NULL);

   delegate_register_isolation_provider(fake_isolation);
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_acquire_refuses_when_isolation_cannot_be_judged\n");
}

static void test_acquire_creates_and_starts_container(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   /* Slice 6: the backend binds aimee-server's UDS only when it is a socket.
    * Point aimee_home at a temp dir and plant a real UDS there. The fake inspect
    * then models aimee_home living in a named volume: Docker must receive the
    * daemon-side volume source, never the in-container path. */
   char tmphome[256];
   snprintf(tmphome, sizeof tmphome, "%s/aimee-deleg-sock-XXXXXX", platform_tmpdir());
   char sockpath[512] = "";
   assert(mkdtemp(tmphome) != NULL);
   setenv("AIMEE_HOME", tmphome, 1);
   snprintf(sockpath, sizeof(sockpath), "%s/aimee-http.sock", tmphome);
   int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(sockfd >= 0);
   struct sockaddr_un addr = {0};
   addr.sun_family = AF_UNIX;
   assert(strlen(sockpath) < sizeof(addr.sun_path));
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
   assert(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
   setenv("AIMEE_FAKE_DOCKER_INSPECT_MOUNTS", "/tmp\t/var/lib/docker/volumes/aimee_home/_data\\n",
          1);

   delegate_backend_config_t cfg = {0};
   cfg.image = "ubuntu:22.04";
   void *state = NULL;
   assert(b->acquire(b, "task-acq-1", &cfg, &state) == 0);
   assert(state != NULL);
   /* The fixture's "create" handler should have touched the .exists
    * flag for the canonical container name. */
   assert(fake_container_exists("aimee-delegate-task-acq-1"));

   /* What the BACKEND contributes is the spec, so that is what is asserted here.
    * The socket path it hands over is already daemon-side: it resolved it by
    * inspecting its own container, and the in-container path must never reach
    * the module (docker would create an empty directory at it). --network none,
    * the mount layering and the environment are the module's rules and are
    * tested against the module rather than through a fake of it. */
   assert(g_launch_calls == 1);
   /* Daemon-side, and the in-container prefix is gone: handing the module the
    * in-container path would have docker create an empty directory there and
    * the delegate's only channel would point at it. */
   assert(strncmp(g_last_socket_host, "/var/lib/docker/volumes/aimee_home/_data/",
                  strlen("/var/lib/docker/volumes/aimee_home/_data/")) == 0);
   assert(strstr(g_last_socket_host, tmphome) == NULL);
   assert(strstr(g_last_socket_host, "/aimee-http.sock") != NULL);
   assert(strstr(g_last_socket_host, tmphome) == NULL);
   assert(strcmp(g_last_launch.parent_socket_target, "/run/aimee/aimee-http.sock") == 0);

   /* A hibernated sandbox from before this fix may still exist with the broken
    * in-container source. Re-acquire must reject that stale bind and recreate the
    * container, rather than resume another round of guaranteed tool timeouts. */
   b->release(b, state, 1);
   assert(fake_create_count() == 1);
   setenv("AIMEE_FAKE_DOCKER_SANDBOX_MOUNTS",
          "/run/aimee/aimee-http.sock\t/var/lib/aimee/aimee-http.sock\\n", 1);
   state = NULL;
   assert(b->acquire(b, "task-acq-1", &cfg, &state) == 0);
   assert(fake_create_count() == 2);

   close(sockfd);
   unlink(sockpath);
   unsetenv("AIMEE_FAKE_DOCKER_SANDBOX_MOUNTS");
   unsetenv("AIMEE_FAKE_DOCKER_INSPECT_MOUNTS");
   unsetenv("AIMEE_HOME");
   rmdir(tmphome);

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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);
   char tmphome[256];
   snprintf(tmphome, sizeof tmphome, "%s/aimee-deleg-hib-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmphome) != NULL);
   setenv("AIMEE_HOME", tmphome, 1);

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
   unsetenv("AIMEE_HOME");
   rmdir(tmphome);
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_release_hibernate_keeps_container\n");
}

static void test_docker_exec_runs_through_fake(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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

static void test_docker_exec_timeout_kills_inner_command(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
   delegate_backend_t *b = delegate_backend_docker_get();
   const char *fixture = write_fake_docker_fixture();
   setenv("AIMEE_DOCKER_BIN", fixture, 1);

   delegate_backend_config_t cfg = {0};
   void *state = NULL;
   assert(b->acquire(b, "task-exec-timeout", &cfg, &state) == 0);

   char sentinel[256];
   assert(snprintf(sentinel, sizeof(sentinel), "/tmp/aimee-docker-timeout-%d", (int)getpid()) <
          (int)sizeof(sentinel));
   unlink(sentinel);
   char command[512];
   assert(snprintf(command, sizeof(command), "sleep 2; : > %s", sentinel) < (int)sizeof(command));
   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   assert(b->exec(b, state, command, 500, &r) == 0);
   assert(r.exit_code != 0);
   assert(r.latency_ms < 1500);
   usleep(750000);
   assert(access(sentinel, F_OK) != 0);

   b->release(b, state, 0);
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: test_docker_exec_timeout_kills_inner_command\n");
}

static void test_docker_exec_set_cwd_prefixes_subsequent_calls(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_register_docker();
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
   delegate_backend_t *b = delegate_backend_docker_get();
   /* Empty task_id rejected. */
   void *state = (void *)0x1;
   assert(b->acquire(b, "", NULL, &state) == -1);
   assert(state == NULL);
   /* NULL state_out rejected. */
   assert(b->acquire(b, "task", NULL, NULL) == -1);
   printf("  PASS: test_acquire_rejects_invalid_args\n");
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
   /* The TREE ITSELF is handed over, not a scratch dir, and a plain checkout is
    * its own repo root with no separate gitdir to find. Whether that becomes
    * `-v <tree>:<tree>` is the module's decision and is tested against the
    * module; what the backend owes is the discovery. */
   assert(strcmp(g_last_worktree, tree) == 0);
   assert(strcmp(g_last_repo, tree) == 0);
   assert(g_last_gitdir[0] == '\0');
   assert(g_last_launch.is_git_checkout == 1);
   assert(g_last_scratch[0] == '\0');
   /* Must run as the server's uid:gid: root-owned files in the user's checkout
    * would be unremovable by them and make git refuse the tree entirely. */
   char uidgid[64];
   snprintf(uidgid, sizeof(uidgid), "%u:%u", (unsigned)getuid(), (unsigned)getgid());
   assert(strcmp(g_last_user, uidgid) == 0);
   b->release(b, state, 0);

   /* read-only reaches the module as writes_allowed=0. Whether that renders as
    * `:ro` is the module's rule -- but a caller that asked for read-only and a
    * module told it may write is the mount losing its enforcement, so the flag
    * itself is pinned here. */
   delegate_backend_config_t rocfg = {0};
   rocfg.workspace = tree;
   rocfg.workspace_read_only = 1;
   void *rostate = NULL;
   assert(b->acquire(b, "task-ws-ro", &rocfg, &rostate) == 0);
   assert(g_last_launch.writes_allowed == 0);
   assert(strcmp(g_last_worktree, tree) == 0);
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
   delegate_register_launch_args_provider(fake_launch_args);
   delegate_register_isolation_provider(fake_isolation);
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
      /* A LINKED worktree: the .git file's `gitdir:` pointer is followed, and the
       * repo root is derived from it. Getting any of these three wrong is how a
       * delegate ends up with a broken git inside a container that looks fine --
       * so what the backend discovered is asserted here. How they are LAYERED
       * (repo read-only beneath, worktree and gitdir writable over it) is the
       * module's rule and is tested against the module. */
      assert(strcmp(g_last_worktree, wt2) == 0);
      assert(strcmp(g_last_repo, repo) == 0);
      assert(strcmp(g_last_gitdir, wtdir) == 0);
      assert(g_last_launch.is_git_checkout == 1);
      assert(g_last_launch.writes_allowed == 1);
      /* the workdir is the worktree, not /workspace */
      assert(g_last_launch.workdir && strcmp(g_last_launch.workdir, wt2) == 0);
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
   /* The RESOLVED path is what is handed over, never the link: the checks ran
    * against the canonical path, and mounting the link instead would mount
    * something the checks never saw. */
   assert(strcmp(g_last_worktree, realrepo) == 0);
   assert(strstr(g_last_worktree, "/link") == NULL);
   b->release(b, state, 0);

   /* A DISJOINT linked worktree — one that lives OUTSIDE its repo root, as a WFE
    * per-slice worktree does ($AIMEE_HOME/wfe-worktrees/...) — is MOUNTED when its
    * two-way git link checks out: the repo goes read-only at its own path, and the
    * worktree and its gitdir are separate writable mounts (no nesting needed). */
   char drepo[300], dwt[300];
   snprintf(drepo, sizeof(drepo), "%s/drepo", base);
   snprintf(dwt, sizeof(dwt), "%s/dwt", base); /* sibling of drepo, NOT nested under it */
   char dwtadmin[500];
   snprintf(dwtadmin, sizeof(dwtadmin), "%s/.git/worktrees/s0", drepo);
   char dcmd[900];
   snprintf(dcmd, sizeof(dcmd), "mkdir -p %s %s", dwtadmin, dwt);
   (void)system(dcmd);
   char dgit[400]; /* forward gitlink: <worktree>/.git -> admin dir under <repo>/.git */
   snprintf(dgit, sizeof(dgit), "%s/.git", dwt);
   FILE *dg = fopen(dgit, "w");
   assert(dg != NULL);
   fprintf(dg, "gitdir: %s\n", dwtadmin);
   fclose(dg);
   char dback[600]; /* backlink: <repo>/.git/worktrees/s0/gitdir -> <worktree>/.git */
   snprintf(dback, sizeof(dback), "%s/gitdir", dwtadmin);
   FILE *dbk = fopen(dback, "w");
   assert(dbk != NULL);
   fprintf(dbk, "%s\n", dgit);
   fclose(dbk);

   cfg.workspace = dwt;
   cfg.workspace_read_only = 0;
   assert(b->acquire(b, "task-disjoint", &cfg, &state) == 0);
   {
      /* A DISJOINT worktree is accepted only because its two-way link checked
       * out, and all three paths are discovered here -- the repo from the
       * gitlink, the worktree as given, the gitdir from the pointer. Whether
       * they are then mounted nested or as separate binds is the module's rule,
       * and does not change what the backend had to find. */
      assert(strcmp(g_last_worktree, dwt) == 0);
      assert(strcmp(g_last_repo, drepo) == 0);
      assert(strcmp(g_last_gitdir, dwtadmin) == 0);
   }
   b->release(b, state, 0);

   /* The SAME disjoint worktree but with a BROKEN backlink (points elsewhere): the
    * backend cannot prove the worktree belongs to the repo, so it refuses rather
    * than mount an out-of-repo tree on a one-way gitlink alone. */
   FILE *dbk2 = fopen(dback, "w");
   assert(dbk2 != NULL);
   fprintf(dbk2, "%s/somewhere-else/.git\n", base);
   fclose(dbk2);
   cfg.workspace = dwt;
   assert(b->acquire(b, "task-disjoint-bad", &cfg, &state) == -1);

   snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
   (void)system(cmd);
   teardown_fake_docker();
   unsetenv("AIMEE_DOCKER_BIN");
   printf("  PASS: docker_workspace_validation_refusals\n");
}

/* reap_aged removes delegate containers older than the threshold and keeps recent
 * ones. Uses a self-contained fake docker: `ps` emits one ancient and one current
 * container line in Go's CreatedAt format; `rm` logs the id it was asked to drop. */
static void test_reap_aged_removes_only_old(void)
{
   char script[256], rmlog[256];
   snprintf(script, sizeof(script), "/tmp/aimee-reap-docker-%d.sh", (int)getpid());
   snprintf(rmlog, sizeof(rmlog), "/tmp/aimee-reap-rmlog-%d", (int)getpid());
   unlink(rmlog);
   FILE *f = fopen(script, "w");
   assert(f != NULL);
   fprintf(f,
           "#!/bin/bash\n"
           "case \"$1\" in\n"
           "  ps)\n"
           "    echo \"aaaaaaaaaaaa aimee-delegate-old 2000-01-01 00:00:00 +0000 UTC\"\n"
           "    echo \"bbbbbbbbbbbb aimee-delegate-new $(date -u +'%%Y-%%m-%%d %%H:%%M:%%S') "
           "+0000 UTC\"\n"
           "    echo \"cccccccccccc some-other-container 2000-01-01 00:00:00 +0000 UTC\"\n"
           "    ;;\n"
           "  rm)\n"
           "    shift 2\n"
           "    echo \"$1\" >> %s\n"
           "    ;;\n"
           "esac\n"
           "exit 0\n",
           rmlog);
   fclose(f);
   assert(chmod(script, 0755) == 0);
   setenv("AIMEE_DOCKER_BIN", script, 1);

   assert(delegate_backend_docker_reap_aged(1800) == 1);

   char buf[256] = {0};
   f = fopen(rmlog, "r");
   assert(f != NULL);
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   assert(strstr(buf, "aaaaaaaaaaaa") != NULL); /* ancient delegate -> reaped */
   assert(strstr(buf, "bbbbbbbbbbbb") == NULL); /* current delegate -> kept */
   /* Ancient, but NOT ours. The container-name prefix is enforced in C now that the
    * `--filter name=^aimee-delegate-` anchor is gone, so this must survive. */
   assert(strstr(buf, "cccccccccccc") == NULL);

   unsetenv("AIMEE_DOCKER_BIN");
   unlink(script);
   unlink(rmlog);
   printf("  PASS: test_reap_aged_removes_only_old\n");
}

int main(void)
{
   test_remove_orphans_accepts_only_container_ids();
   test_reap_aged_removes_only_old();
   printf("delegate_backend_docker:\n");
   test_register_puts_docker_in_registry();
   test_file_ops_reject_null_state();
   test_translate_named_volume_socket_path();
   test_build_exec_command_basic();
   test_build_exec_command_handles_special_chars();
   test_build_exec_command_rejects_invalid();
   test_acquire_creates_and_starts_container();
   test_acquire_refuses_when_isolation_is_refused();
   test_acquire_refuses_when_isolation_cannot_be_judged();
   test_release_hibernate_keeps_container();
   test_docker_exec_runs_through_fake();
   test_docker_exec_propagates_nonzero_exit();
   test_docker_exec_timeout_kills_inner_command();
   test_docker_exec_set_cwd_prefixes_subsequent_calls();
   test_acquire_rejects_invalid_args();
   test_docker_write_then_read_roundtrip();
   test_read_file_returns_only_the_file();
   test_docker_absolute_in_workspace_path_accepted();
   test_docker_path_validation_rejects_escapes();
   test_docker_list_dir_returns_entries();
   test_docker_mounts_caller_workspace();
   test_docker_refuses_a_missing_workspace();
   test_docker_workspace_validation_refusals();
   printf("ok\n");
   return 0;
}
