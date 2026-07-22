/* test_workspace_provider_container.c: the container workspace provider.
 *
 * This is the seam the delegate sandbox stands on: bind one of these for a
 * delegate's turn and td_bash / td_read_file / td_write_file / td_list_files run
 * INSIDE its container, because those tools already resolve through
 * workspace_provider_active(). Unbound, td_bash falls through to run_cmd —
 * in-process, inside aimee-server, with the server's filesystem and environment.
 * So every op here is the difference between a sandboxed delegate and one that
 * merely believes it is sandboxed.
 *
 * The backend is faked, so this needs no docker: what is under test is the
 * marshalling (argv quoting, the read/write/list/stat mapping, error propagation),
 * not the container runtime. */
#include "aimee.h"
#include "workspace_provider_container.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── fake backend ─────────────────────────────────────────────────────────── */

static char g_last_cmd[4096];
static int g_exec_calls;
static int g_exec_rc;   /* what exec() returns (transport) */
static int g_exec_exit; /* the exit code it reports */
static const char *g_stdout_text;
static const char *g_stderr_text;
static char g_last_write_path[256];
static char g_last_write_content[256];

static int fake_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                     delegate_exec_result_t *r)
{
   (void)self;
   (void)state;
   (void)timeout_ms;
   g_exec_calls++;
   snprintf(g_last_cmd, sizeof(g_last_cmd), "%s", command ? command : "");
   if (g_exec_rc != 0)
      return g_exec_rc;
   if (r)
   {
      r->exit_code = g_exec_exit;
      if (g_stdout_text && r->stdout_buf)
         snprintf(r->stdout_buf, r->stdout_cap, "%s", g_stdout_text);
      if (g_stderr_text && r->stderr_buf)
         snprintf(r->stderr_buf, r->stderr_cap, "%s", g_stderr_text);
   }
   return 0;
}

static int fake_read_file(delegate_backend_t *self, void *state, const char *path, int offset,
                          int limit, char **out)
{
   (void)self;
   (void)state;
   (void)offset;
   (void)limit;
   if (!out)
      return -1;
   if (strcmp(path, "missing.c") == 0)
      return -1;
   *out = safe_strdup("int main(void) { return 0; }");
   return 0;
}

static int fake_write_file(delegate_backend_t *self, void *state, const char *path,
                           const char *content)
{
   (void)self;
   (void)state;
   snprintf(g_last_write_path, sizeof(g_last_write_path), "%s", path ? path : "");
   snprintf(g_last_write_content, sizeof(g_last_write_content), "%s", content ? content : "");
   return 0;
}

static int fake_list_dir(delegate_backend_t *self, void *state, const char *path, char ***out)
{
   (void)self;
   (void)state;
   (void)path;
   if (!out)
      return -1;
   char **e = calloc(4, sizeof(*e));
   e[0] = safe_strdup("main.c");
   e[1] = safe_strdup("README.md");
   e[2] = safe_strdup("util.c");
   e[3] = NULL;
   *out = e;
   return 0;
}

static delegate_backend_t g_fake = {.name = "fake",
                                    .description = "test backend",
                                    .acquire = NULL,
                                    .release = NULL,
                                    .exec = fake_exec,
                                    .read_file = fake_read_file,
                                    .write_file = fake_write_file,
                                    .list_dir = fake_list_dir,
                                    .get_cwd = NULL,
                                    .set_cwd = NULL};

static int g_state = 1; /* a non-NULL handle */

static void reset(void)
{
   g_exec_calls = 0;
   g_exec_rc = 0;
   g_exec_exit = 0;
   g_stdout_text = NULL;
   g_stderr_text = NULL;
   g_last_cmd[0] = g_last_write_path[0] = g_last_write_content[0] = '\0';
}

static const workspace_provider_t *bind_provider(ws_container_provider_t *inst)
{
   assert(ws_container_provider_init(inst, &g_fake, &g_state) == 0);
   return &inst->base;
}

/* ── tests ────────────────────────────────────────────────────────────────── */

/* A half-bound provider must be refused. Every op on one would fall through to the
 * host, so handing one out would silently un-sandbox the delegate — the failure
 * this whole seam exists to prevent. */
static void test_init_refuses_a_half_bound_provider(void)
{
   ws_container_provider_t inst;
   assert(ws_container_provider_init(NULL, &g_fake, &g_state) == -1);
   assert(ws_container_provider_init(&inst, NULL, &g_state) == -1);
   assert(ws_container_provider_init(&inst, &g_fake, NULL) == -1);
   assert(ws_container_provider_init(&inst, &g_fake, &g_state) == 0);
   assert(inst.base.kind == WS_PROVIDER_CONTAINER);
   /* The kind must NAME itself: reported as "shared", a sandboxed delegate would be
    * described as running on the host. */
   assert(strcmp(ws_provider_kind_to_string(inst.base.kind), "container") == 0);
   /* And it must not be reachable from config: "container" resolving to a kind
    * would mean a workspace could ask for it and silently get `shared`. */
   assert(ws_provider_kind_from_string("container") == WS_PROVIDER_SHARED);
   printf("  PASS: init_refuses_a_half_bound_provider\n");
}

/* argv must reach the backend as a properly quoted command line. The caller passed
 * argv to AVOID a shell; the backend takes a string, so this is the exact point
 * where an argument containing a space or a `;` could become a second command. */
static void test_exec_argv_is_shell_quoted(void)
{
   reset();
   ws_container_provider_t inst;
   const workspace_provider_t *p = bind_provider(&inst);

   const char *argv[] = {"grep", "-n", "hello world", "src/a b.c", NULL};
   char *out = NULL;
   p->exec(p, argv, &out, 4096);
   assert(g_exec_calls == 1);
   /* Each argument single-quoted: the space must not split an argument. */
   assert(strstr(g_last_cmd, "'hello world'") != NULL);
   assert(strstr(g_last_cmd, "'src/a b.c'") != NULL);
   free(out);

   /* The injection case: a quote and a `;` must not escape into a second command. */
   reset();
   const char *evil[] = {"echo", "x'; rm -rf /tmp/pwned; echo '", NULL};
   p->exec(p, evil, &out, 4096);
   /* The payload survives as DATA inside quotes, never as an unquoted `;`. */
   assert(strstr(g_last_cmd, "rm -rf /tmp/pwned") != NULL); /* present... */
   assert(strstr(g_last_cmd, "'\\''") != NULL);             /* ...but escaped */
   free(out);
   printf("  PASS: exec_argv_is_shell_quoted\n");
}

/* Combined stdout+stderr, per the contract. A delegate whose build fails needs the
 * compiler's message; dropping stderr turns a diagnosable failure into a bare
 * exit code. */
static void test_exec_shell_combines_output_and_reports_exit(void)
{
   reset();
   ws_container_provider_t inst;
   const workspace_provider_t *p = bind_provider(&inst);

   g_exec_exit = 2;
   g_stdout_text = "compiling";
   g_stderr_text = "error: undefined reference";
   int code = -99;
   char *out = p->exec_shell(p, "make all", &code);
   assert(out != NULL);
   assert(code == 2); /* the command's exit code, not the transport's */
   assert(strstr(out, "compiling") != NULL);
   assert(strstr(out, "undefined reference") != NULL); /* stderr kept */
   free(out);
   printf("  PASS: exec_shell_combines_output_and_reports_exit\n");
}

/* A transport failure (the container is gone) must be distinguishable from a
 * command that ran and failed. Conflating them would tell a delegate its build
 * failed when in fact its sandbox died. */
static void test_transport_failure_is_not_a_nonzero_exit(void)
{
   reset();
   ws_container_provider_t inst;
   const workspace_provider_t *p = bind_provider(&inst);

   g_exec_rc = -1; /* backend could not run it at all */
   int code = 0;
   char *out = p->exec_shell(p, "true", &code);
   assert(code == -1);
   assert(out == NULL);
   free(out);
   printf("  PASS: transport_failure_is_not_a_nonzero_exit\n");
}

static void test_read_write_map_to_the_backend(void)
{
   reset();
   ws_container_provider_t inst;
   const workspace_provider_t *p = bind_provider(&inst);

   char *buf = NULL;
   size_t len = 0;
   assert(p->read_all(p, "main.c", &buf, &len) == 0);
   assert(buf && strstr(buf, "int main") != NULL);
   assert(len == strlen(buf));
   free(buf);

   /* A read failure must surface, not yield an empty-but-successful file. */
   buf = (char *)0x1;
   assert(p->read_all(p, "missing.c", &buf, &len) == -1);
   assert(buf == NULL);

   assert(p->write_all(p, "out.c", "hello", 5) == 0);
   assert(strcmp(g_last_write_path, "out.c") == 0);
   assert(strcmp(g_last_write_content, "hello") == 0);

   /* len 0 with NULL data writes an empty file, per the contract. */
   assert(p->write_all(p, "empty.c", NULL, 0) == 0);
   assert(strcmp(g_last_write_content, "") == 0);
   printf("  PASS: read_write_map_to_the_backend\n");
}

/* The backend's write_file is NUL-terminated, so binary content would be silently
 * truncated at the first NUL. Refusing is the point: a delegate told its binary
 * write succeeded would corrupt the file and find out much later. */
static void test_binary_write_is_refused_not_truncated(void)
{
   reset();
   ws_container_provider_t inst;
   const workspace_provider_t *p = bind_provider(&inst);

   const char payload[] = {'E', 'L', 'F', '\0', 'x', 'y'};
   assert(p->write_all(p, "a.out", payload, sizeof(payload)) == -1);
   /* And it must not have written a truncated prefix on the way out. */
   assert(g_last_write_path[0] == '\0');
   printf("  PASS: binary_write_is_refused_not_truncated\n");
}

static void test_list_globs_and_stat_reads_kind_and_size(void)
{
   reset();
   ws_container_provider_t inst;
   const workspace_provider_t *p = bind_provider(&inst);

   char **entries = NULL;
   int n = 0;
   assert(p->list(p, ".", "*.c", &entries, &n) == 0);
   assert(n == 2); /* main.c + util.c; README.md filtered out */
   ws_provider_free_list(entries, n);

   assert(p->list(p, ".", NULL, &entries, &n) == 0);
   assert(n == 3); /* no pattern = everything */
   ws_provider_free_list(entries, n);

   /* stat: a file reports its size... */
   reset();
   g_stdout_text = "f 1234";
   ws_stat_t st;
   assert(p->stat(p, "main.c", &st) == 0);
   assert(st.exists == 1 && st.is_dir == 0 && st.size == 1234);

   /* ...a directory reports is_dir... */
   reset();
   g_stdout_text = "d -";
   assert(p->stat(p, "src", &st) == 0);
   assert(st.exists == 1 && st.is_dir == 1);

   /* ...and an absent path is absent, not an error. */
   reset();
   g_stdout_text = "x -";
   assert(p->stat(p, "nope", &st) == 0);
   assert(st.exists == 0);
   printf("  PASS: list_globs_and_stat_reads_kind_and_size\n");
}

int main(void)
{
   printf("test_workspace_provider_container:\n");
   test_init_refuses_a_half_bound_provider();
   test_exec_argv_is_shell_quoted();
   test_exec_shell_combines_output_and_reports_exit();
   test_transport_failure_is_not_a_nonzero_exit();
   test_read_write_map_to_the_backend();
   test_binary_write_is_refused_not_truncated();
   test_list_globs_and_stat_reads_kind_and_size();
   printf("All workspace_provider_container tests passed.\n");
   return 0;
}
