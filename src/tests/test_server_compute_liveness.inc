static void test_background_ok_rejects_raw_tool_call_markup(void)
{
   int job_id =
       db1_agent_job_create("explain", "run raw tool-call regression", "minimax", "unit-test");
   assert(job_id > 0);

   pthread_mutex_t mu;
   pthread_mutex_init(&mu, NULL);
   compute_ctx_t done_ctx = {.conn_fd = -1, .background_job_id = job_id, .write_mutex = &mu};
   cJSON *done = cJSON_CreateObject();
   cJSON_AddStringToObject(done, "status", "ok");
   cJSON_AddStringToObject(done, "response",
                           "I will inspect it.\n[TOOL_CALL]\n{\"tool\":\"bash\"}\n[/TOOL_CALL]");
   cJSON_AddNumberToObject(done, "turns", 0);
   compute_respond(&done_ctx, done);

   db1_agent_job_t job;
   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "failed") == 0);
   assert(strstr(job.result, "degenerate response") != NULL);
   pthread_mutex_destroy(&mu);

   printf("  PASS: test_background_ok_rejects_raw_tool_call_markup\n");
}

static void test_background_ok_rejects_unexecuted_tool_plan(void)
{
   int job_id = db1_agent_job_create("explain", "run unexecuted command plan regression", "minimax",
                                     "unit-test");
   assert(job_id > 0);

   pthread_mutex_t mu;
   pthread_mutex_init(&mu, NULL);
   compute_ctx_t done_ctx = {.conn_fd = -1, .background_job_id = job_id, .write_mutex = &mu};
   cJSON *done = cJSON_CreateObject();
   cJSON_AddStringToObject(done, "status", "ok");
   cJSON_AddStringToObject(done, "response",
                           "I'll read the proposal first, then inspect the code.\n"
                           "```\n"
                           "sed -n '1,220p' docs/proposals/accepted/example.md\n"
                           "```\n"
                           "```\n"
                           "rg -n \"calibration\" src tests\n"
                           "```\n");
   cJSON_AddNumberToObject(done, "turns", 0);
   cJSON_AddNumberToObject(done, "tool_calls", 0);
   compute_respond(&done_ctx, done);

   db1_agent_job_t job;
   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "failed") == 0);
   assert(strstr(job.result, "unexecuted tool-use plan") != NULL);
   pthread_mutex_destroy(&mu);

   printf("  PASS: test_background_ok_rejects_unexecuted_tool_plan\n");
}

static void test_direct_delegate_rejects_raw_tool_call_markup(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "I will inspect it.\n[TOOL_CALL]\n{\"tool\":\"bash\"}\n[/TOOL_CALL]";

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "explain");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run raw tool-call regression");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;

   close(fds[1]);
   char buf[8192];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   /* (WP-B) async-only: the degenerate-response rejection is persisted to the
    * job row (status failed; message in result). */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strstr(job.result, "degenerate response") != NULL);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);

   printf("  PASS: test_direct_delegate_rejects_raw_tool_call_markup\n");
}
