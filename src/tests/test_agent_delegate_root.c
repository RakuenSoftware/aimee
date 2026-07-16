/* test_agent_delegate_root.c: focused delegate root-cause regressions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "db1.h"
#include "agent_tasks.h"
#include "agent_tools.h"
#include "workspace_provider.h"
#include "cJSON.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "util.h"

void test_cancelled_durable_job_blocks_tool_dispatch(void)
{
   db1_shutdown();
   assert(db1_init(":memory:") == 0);

   int job_id = db1_agent_job_create("code", "write a file", "unit-agent", "unit-test");
   assert(job_id > 0);
   assert(db1_agent_job_take_lease(job_id, "unit-test") == 0);
   assert(db1_agent_job_cancel_by_id(job_id, "unit test") > 0);
   agent_set_durable_job(job_id);

   char path[512];
   snprintf(path, sizeof(path), "%s/aimee-cancelled-dispatch-%ld.txt", platform_tmpdir(),
            (long)getpid());
   unlink(path);

   char args[1024];
   snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"should not write\"}", path);
   char *result = dispatch_tool_call("write_file", args, 1000);
   assert(result != NULL);
   assert(strstr(result, "delegate cancelled") != NULL);
   assert(access(path, F_OK) != 0);

   free(result);
   agent_set_durable_job(0);
   db1_shutdown();
   printf("  PASS: test_cancelled_durable_job_blocks_tool_dispatch\n");
}

void test_parent_write_guard_readonly_pipeline(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_pipe_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   char source[512];
   snprintf(source, sizeof(source), "%s/pipe-source.txt", worktree);
   FILE *f = fopen(source, "w");
   assert(f != NULL);
   fputs("first\nsecond\n", f);
   fclose(f);

   agent_tools_parent_write_guard_set(root, worktree);
   run_cmd_set_cwd(worktree);
   char *result = tool_bash("cat pipe-source.txt | head -n 1", 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strcmp(stdout_item->valuestring, "first\n") == 0);
   assert(ec && ec->valueint == 0);

   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(root);
   printf("  PASS: test_parent_write_guard_readonly_pipeline\n");
}

/* Regression: a wfe delegate runs on a pooled worker thread and inherits the
 * process-global parent-write guard left by a PRIOR run (only the primary chat
 * path sets it per-run). If that stale guard's read-only root encloses the
 * delegate's OWN worktree while its write root points at a DIFFERENT worktree, a
 * native write_file into the delegate's worktree is wrongly rejected as "parent
 * read-only" — the implement delegate commits an empty tree (a no-op round).
 * wfe_live_delegate_run now clears the guard before running; this pins the
 * clear-unblocks-the-write behavior the fix relies on. */
void test_stale_parent_guard_blocks_other_worktree_then_clear_unblocks(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_stale_guard_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   /* Two sibling worktrees under a common read-only root, exactly the shape a
    * prior primary session (pinned to worktree A) leaves behind when a wfe
    * delegate then runs in worktree B. */
   char wt_a[512], wt_b[512];
   snprintf(wt_a, sizeof(wt_a), "%s/.aimee/worktrees/sessionA/main", root);
   snprintf(wt_b, sizeof(wt_b), "%s/.aimee/worktrees/wfeB/main", root);
   assert(platform_mkdir_p(wt_a, 0700) == 0 || access(wt_a, F_OK) == 0);
   assert(platform_mkdir_p(wt_b, 0700) == 0 || access(wt_b, F_OK) == 0);

   /* Stale guard from the prior session: read-only root = the common parent,
    * write root = worktree A. */
   agent_tools_parent_write_guard_set(root, wt_a);

   /* The wfe delegate, running in worktree B, tries to write there. Under the
    * stale guard this is blocked (B is under the ro root but not under A). */
   run_cmd_set_cwd(wt_b);
   char *blocked = tool_write_file("new_impl.txt", "codex output\n");
   run_cmd_set_cwd(NULL);
   assert(blocked != NULL);
   assert(strstr(blocked, "blocked") != NULL); /* rejected as parent read-only */
   assert(access("/dev/null", F_OK) == 0);     /* sanity: fs reachable */
   char wrote_path[600];
   snprintf(wrote_path, sizeof(wrote_path), "%s/new_impl.txt", wt_b);
   assert(access(wrote_path, F_OK) != 0); /* nothing landed */
   free(blocked);

   /* The fix: clear the stale guard (also restores write capability). */
   agent_tools_parent_write_guard_clear();

   run_cmd_set_cwd(wt_b);
   char *ok = tool_write_file("new_impl.txt", "codex output\n");
   run_cmd_set_cwd(NULL);
   assert(ok != NULL);
   assert(strstr(ok, "blocked") == NULL); /* now allowed */
   assert(access(wrote_path, F_OK) == 0); /* the write landed in worktree B */

   free(ok);
   platform_test_rmrf(root);
   printf("  PASS: test_stale_parent_guard_blocks_other_worktree_then_clear_unblocks\n");
}

void test_parent_write_guard_readonly_large_find(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_find_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   char many_dir[512];
   snprintf(many_dir, sizeof(many_dir), "%s/many", worktree);
   assert(platform_mkdir_p(many_dir, 0700) == 0);
   for (int i = 0; i < 300; i++)
   {
      char path[512];
      snprintf(path, sizeof(path), "%s/file-%04d.txt", many_dir, i);
      FILE *f = fopen(path, "w");
      assert(f != NULL);
      fputs("x", f);
      fclose(f);
   }

   agent_tools_parent_write_guard_set(root, worktree);
   run_cmd_set_cwd(worktree);
   char cmd[1024];
   /* Sort the listing so the highest-numbered file is deterministically last.
    * tool_bash compacts large output to a head + tail window, so an unsorted
    * `find` (filesystem order) could leave file-0299.txt in the omitted middle;
    * sorting puts it in the preserved tail and keeps the assertion stable. */
   snprintf(cmd, sizeof(cmd), "find %s -maxdepth 2 | sort", worktree);
   char *result = tool_bash(cmd, 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   /* The read-only find must be allowed (not blocked) and complete: its
    * highest-numbered entry survives in the compacted tail. */
   assert(strstr(stdout_item->valuestring, "file-0299.txt") != NULL);
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(root);
   printf("  PASS: test_parent_write_guard_readonly_large_find\n");
}

typedef struct cancel_job_args
{
   int job_id;
} cancel_job_args_t;

static void *cancel_job_soon(void *arg)
{
   cancel_job_args_t *args = (cancel_job_args_t *)arg;
   usleep(150000);
   db1_agent_job_cancel_by_id(args->job_id, "unit test running cancel");
   return NULL;
}

void test_delegate_bash_cancel_kills_running_tool(void)
{
   db1_shutdown();
   assert(db1_init(":memory:") == 0);

   int job_id = db1_agent_job_create("diagnose", "run sleep", "unit-agent", "unit-test");
   assert(job_id > 0);
   assert(db1_agent_job_take_lease(job_id, "unit-test") == 0);
   agent_set_durable_job(job_id);

   cancel_job_args_t args = {.job_id = job_id};
   pthread_t tid;
   assert(pthread_create(&tid, NULL, cancel_job_soon, &args) == 0);
   char *result = tool_bash("sleep 5", 5000);
   pthread_join(tid, NULL);
   agent_set_durable_job(0);

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   cJSON *stderr_item = cJSON_GetObjectItem(json, "stderr");
   assert(ec && ec->valueint == -1);
   assert(stderr_item && cJSON_IsString(stderr_item));
   assert(strstr(stderr_item->valuestring, "delegate cancelled") != NULL);
   cJSON_Delete(json);
   free(result);
   db1_shutdown();
   printf("  PASS: test_delegate_bash_cancel_kills_running_tool\n");
}

void test_parent_write_guard_allows_mkdir_in_delegate_worktree(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_mkdir_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   agent_tools_parent_write_guard_set(root, worktree);
   run_cmd_set_cwd(worktree);
   char *result = tool_bash("mkdir -p new-fixtures/subdir", 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char created[512];
   snprintf(created, sizeof(created), "%s/new-fixtures/subdir", worktree);
   assert(access(created, F_OK) == 0);

   platform_test_rmrf(root);
   printf("  PASS: test_parent_write_guard_allows_mkdir_in_delegate_worktree\n");
}

void test_parent_write_guard_allows_workspace_file_ops(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_file_ops_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   agent_tools_parent_write_guard_set(root, worktree);
   run_cmd_set_cwd(worktree);
   char *result = tool_bash("touch marker.txt", 5000);
   run_cmd_set_cwd(NULL);

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char marker[512];
   snprintf(marker, sizeof(marker), "%s/marker.txt", worktree);
   assert(access(marker, F_OK) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("cp marker.txt marker-copy.txt", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char marker_copy[512];
   snprintf(marker_copy, sizeof(marker_copy), "%s/marker-copy.txt", worktree);
   assert(access(marker_copy, F_OK) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("mv marker-copy.txt marker-moved.txt", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char marker_moved[512];
   snprintf(marker_moved, sizeof(marker_moved), "%s/marker-moved.txt", worktree);
   assert(access(marker_moved, F_OK) == 0);

   run_cmd_set_cwd(worktree);
   result = tool_bash("rm marker-moved.txt", 5000);
   run_cmd_set_cwd(NULL);
   assert(result != NULL);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);
   assert(access(marker_moved, F_OK) != 0);

   char outside[512];
   snprintf(outside, sizeof(outside), "%s/outside.txt", root);
   char command[1024];
   snprintf(command, sizeof(command), "touch %s", outside);
   run_cmd_set_cwd(worktree);
   result = tool_bash(command, 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();

   assert(result != NULL);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == -1);
   cJSON_Delete(json);
   free(result);
   assert(access(outside, F_OK) != 0);

   platform_test_rmrf(root);
   printf("  PASS: test_parent_write_guard_allows_workspace_file_ops\n");
}

void test_parent_write_guard_allows_workspace_chain(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_chain_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   agent_tools_parent_write_guard_set(root, worktree);
   run_cmd_set_cwd(worktree);
   char *result = tool_bash("mkdir -p chain && touch chain/ok.txt", 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   char marker[512];
   snprintf(marker, sizeof(marker), "%s/chain/ok.txt", worktree);
   assert(access(marker, F_OK) == 0);

   platform_test_rmrf(root);
   printf("  PASS: test_parent_write_guard_allows_workspace_chain\n");
}

void test_parent_write_guard_allows_readonly_printf(void)
{
   char root[512];
   snprintf(root, sizeof(root), "%s/aimee_parent_guard_printf_XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/.aimee/worktrees/delegate/main", root);
   assert(platform_mkdir_p(worktree, 0700) == 0 || access(worktree, F_OK) == 0);

   agent_tools_parent_write_guard_set(root, worktree);
   run_cmd_set_cwd(worktree);
   char *result = tool_bash("printf LOCAL254_TOOLS_OK", 5000);
   run_cmd_set_cwd(NULL);
   agent_tools_parent_write_guard_clear();

   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *stdout_item = cJSON_GetObjectItem(json, "stdout");
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(stdout_item && cJSON_IsString(stdout_item));
   assert(strcmp(stdout_item->valuestring, "LOCAL254_TOOLS_OK") == 0);
   assert(ec && ec->valueint == 0);

   cJSON_Delete(json);
   free(result);
   platform_test_rmrf(root);
   printf("  PASS: test_parent_write_guard_allows_readonly_printf\n");
}

/* A DETACHED workspace whose serving client has disconnected (e.g. any
 * background/durable delegate) has a dead reverse channel: exec_shell returns
 * NULL. Fix A requires this surface as a clear error, NOT a silent
 * {"stdout":"","stderr":"","exit_code":-1} that reads like the command ran and
 * failed. (A real command with empty output returns "" — non-NULL.) */
static char *dead_channel_exec_shell(const workspace_provider_t *p, const char *cmd, int *exit_code)
{
   (void)p;
   (void)cmd;
   if (exit_code)
      *exit_code = -1;
   return NULL; /* no usable reverse-channel response — client gone */
}

void test_detached_dead_channel_reports_clear_error(void)
{
   workspace_provider_t mock;
   memset(&mock, 0, sizeof(mock));
   mock.kind = WS_PROVIDER_DETACHED;
   mock.exec_shell = dead_channel_exec_shell;
   workspace_provider_set_active(&mock);

   char *result = tool_bash("echo hi", 5000);
   workspace_provider_clear_active();

   assert(result != NULL);
   assert(strstr(result, "reverse-channel unavailable") != NULL);
   assert(strstr(result, "serving client is not connected") != NULL);

   /* Must not masquerade as a normal empty-output command result: stderr is set. */
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *stderr_item = cJSON_GetObjectItem(json, "stderr");
   assert(stderr_item && cJSON_IsString(stderr_item) && stderr_item->valuestring[0] != '\0');
   cJSON_Delete(json);
   free(result);
   printf("  PASS: test_detached_dead_channel_reports_clear_error\n");
}

/* A CONTAINER-sandboxed delegate must run its shell/script INSIDE the container via
 * the provider's exec_shell — NOT as a local fork on the aimee-server host, which
 * would execute the model's arbitrary command on the host (its filesystem, its
 * network) and escape the `--network none` sandbox. The file tools already route in;
 * bash/execute_script were the hole. The spy stands in for the container: if either
 * tool still forked locally, the spy would never be called. */
static int g_sbx_exec_called;
static char *g_sbx_exec_cmd;
static char *sandbox_capture_exec_shell(const workspace_provider_t *p, const char *cmd,
                                        int *exit_code)
{
   (void)p;
   g_sbx_exec_called = 1;
   free(g_sbx_exec_cmd);
   g_sbx_exec_cmd = cmd ? strdup(cmd) : NULL;
   if (exit_code)
      *exit_code = 0;
   return strdup("SANDBOX-STDOUT-MARKER");
}

void test_container_bash_runs_in_sandbox(void)
{
   workspace_provider_t mock;
   memset(&mock, 0, sizeof(mock));
   mock.kind = WS_PROVIDER_CONTAINER;
   mock.exec_shell = sandbox_capture_exec_shell;
   workspace_provider_set_active(&mock);
   g_sbx_exec_called = 0;
   free(g_sbx_exec_cmd);
   g_sbx_exec_cmd = NULL;

   char *result = tool_bash("echo hi", 5000);
   workspace_provider_clear_active();

   /* Routed INTO the container (spy hit), carrying our command. */
   assert(g_sbx_exec_called == 1);
   assert(g_sbx_exec_cmd && strstr(g_sbx_exec_cmd, "echo hi"));
   cJSON *j = cJSON_Parse(result);
   assert(j);
   cJSON *so = cJSON_GetObjectItem(j, "stdout");
   assert(so && cJSON_IsString(so) && strstr(so->valuestring, "SANDBOX-STDOUT-MARKER"));
   cJSON_Delete(j);
   free(result);
   free(g_sbx_exec_cmd);
   g_sbx_exec_cmd = NULL;
   printf("  PASS: test_container_bash_runs_in_sandbox\n");
}

void test_container_execute_script_runs_in_sandbox(void)
{
   workspace_provider_t mock;
   memset(&mock, 0, sizeof(mock));
   mock.kind = WS_PROVIDER_CONTAINER;
   mock.exec_shell = sandbox_capture_exec_shell;
   workspace_provider_set_active(&mock);
   g_sbx_exec_called = 0;
   free(g_sbx_exec_cmd);
   g_sbx_exec_cmd = NULL;

   char *result =
       dispatch_tool_call("execute_script", "{\"language\":\"bash\",\"body\":\"echo hi\"}", 5000);
   workspace_provider_clear_active();

   assert(g_sbx_exec_called == 1);
   assert(g_sbx_exec_cmd && strstr(g_sbx_exec_cmd, "echo hi"));
   /* Fed over a quoted heredoc so the body needs no escaping. */
   assert(strstr(g_sbx_exec_cmd, "AIMEE_SCRIPT_EOF"));
   cJSON *j = cJSON_Parse(result);
   assert(j);
   cJSON *so = cJSON_GetObjectItem(j, "stdout");
   assert(so && cJSON_IsString(so) && strstr(so->valuestring, "SANDBOX-STDOUT-MARKER"));
   cJSON_Delete(j);
   free(result);
   free(g_sbx_exec_cmd);
   g_sbx_exec_cmd = NULL;
   printf("  PASS: test_container_execute_script_runs_in_sandbox\n");
}
