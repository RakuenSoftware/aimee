static void test_delegate_launch_repairs_paths_from_request_cwd(void)
{
   reset_last_response();
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   char cwd[MAX_PATH_LEN];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "cwd", cwd);
   cJSON *plan = cJSON_AddObjectToObject(req, "plan");
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON_AddStringToObject(plan, "title", "Repair Launch Plan");
   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");
   add_launch_packet(packets, "packet-repair", "Repair path", "test_server_compute.c");

   assert(handle_delegate_launch(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   int job_id = cJSON_GetObjectItem(g_last_response, "job_id")->valueint;
   assert(job_id > 0);

   db1_coord_task_t tasks[2];
   int count = db1_coord_job_list_tasks(job_id, tasks, 2);
   assert(count == 1);
   assert(strstr(tasks[0].files, "src/tests/test_server_compute.c") != NULL);

   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_launch_repairs_paths_from_request_cwd\n");
}

/* (WP-B) test_noop_write_delegate_fires deleted: it asserted foreground
 * write-delegate semantics (no worktree, no-op detection on the parent tree).
 * Async-only delegates always isolate; this foreground-shaped case is obsolete.
 * No-op detection itself is exercised via delegate_run_phases unit coverage. */

/* Diff grounding: inspection delegates must receive the evidence bundle in
 * their task prompt while reading from the parent workspace by default. */
static void assert_role_gets_evidence_bundle_with_cwd(const char *role, const char *cwd)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "Validation complete — no findings";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", "/tmp/aimee-parent-repo");
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s",
            "/tmp/aimee-delegate-wt");
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", role);
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "validate the changes in the current diff for correctness and coverage");
   if (cwd)
      cJSON_AddStringToObject(req, "cwd", cwd);
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[4096];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n > 0);
   buf[n] = '\0';
   close(fds[0]);
   assert(strstr(g_last_agent_prompt, "Parent Worktree Diff Evidence") != NULL);
   assert(strstr(g_last_agent_prompt, "Validation Evidence Bundle") != NULL);
   assert(g_worktree_create_calls == 0);
   assert(g_worktree_apply_calls == 0);
   /* (WP-B) async-only: the worker's status/launch_worktree_path are persisted to
    * the job row, not delivered over the connection (the pipe carries the
    * {job_id,pending} envelope). The read-only delegate ran in the parent
    * workspace (asserted above via the evidence bundle + zero worktree calls). */
   db1_agent_job_t job;
   assert(delegate_envelope_job(buf, &job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
}

static void assert_role_gets_evidence_bundle(const char *role)
{
   assert_role_gets_evidence_bundle_with_cwd(role, "/tmp");
}

static void test_read_only_delegate_uses_parent_workspace(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "Read-only validation complete";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", "/tmp/aimee-parent-repo");
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s",
            "/tmp/aimee-delegate-wt");

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "validate");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "Read-only. Validate the changes in the current diff for correctness.");
   cJSON_AddStringToObject(req, "cwd", "/tmp/aimee-parent-repo");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);

   char buf[4096];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n > 0);
   buf[n] = '\0';
   close(fds[0]);

   assert(g_worktree_create_calls == 0); /* read-only: no sibling worktree */
   assert(g_worktree_apply_calls == 0);
   assert(strstr(g_last_agent_prompt, "Validation Evidence Bundle") != NULL);
   /* (WP-B) async-only: read the persisted job rather than the connection. */
   db1_agent_job_t job;
   assert(delegate_envelope_job(buf, &job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_read_only_delegate_uses_parent_workspace\n");
}

static void test_read_only_branch_delegate_rejected(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "should not run";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", "/tmp/aimee-parent-repo");
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s",
            "/tmp/aimee-delegate-wt");

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "validate");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "Read-only. Validate the changes in the current diff for correctness.");
   cJSON_AddStringToObject(req, "cwd", "/tmp/aimee-parent-repo");
   cJSON_AddStringToObject(req, "branch", "feature/read-only-review");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);

   char buf[4096];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n > 0);
   buf[n] = '\0';
   close(fds[0]);

   assert(g_worktree_create_calls == 0);
   assert(g_agent_run_calls == 0); /* rejected before the agent ran */
   assert(g_agent_tool_run_calls == 0);
   /* (WP-B) async-only: the rejection error is persisted to the job row. */
   db1_agent_job_t job;
   assert(delegate_envelope_job(buf, &job) == 0);
   assert(strstr(job.result, "read-only delegates must use the parent worktree") != NULL);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_read_only_branch_delegate_rejected\n");
}

/* (WP-B) test_write_delegate_without_named_paths_noops deleted: foreground
 * write-delegate no-op semantics, obsolete under always-isolate async. */

static void test_inspection_roles_get_evidence_bundle(void)
{
   assert_role_gets_evidence_bundle("validate");
   assert_role_gets_evidence_bundle("review");
   assert_role_gets_evidence_bundle("diagnose");
   assert_role_gets_evidence_bundle_with_cwd("review", NULL);
   printf("  PASS: test_inspection_roles_get_evidence_bundle\n");
}
