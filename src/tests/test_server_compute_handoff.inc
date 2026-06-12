/* test_server_compute_handoff.inc: direct-delegate handoff / tools / max-turns
 * tests split out of test_server_compute.c to keep that .c under the 2000-line
 * hard limit. Included (same translation unit) after the impl includes so the
 * white-box statics and stubs above stay in scope. */
static void test_direct_delegate_handoff_json_response(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "{"
                      "\"schema_version\":\"delegate_result_v1\","
                      "\"status\":\"done\","
                      "\"changed_files\":[],"
                      "\"tests\":[{\"name\":\"unit-test-server-compute\",\"status\":\"passed\"}],"
                      "\"supervisor_actions\":[],"
                      "\"summary\":\"server handoff ok\""
                      "}";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the direct delegate handoff json test prompt");
   cJSON_AddTrueToObject(req, "handoff_json");
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
   /* (WP-B) async-only: status+result persist to the job row; the rich handoff_*
    * sync-response metadata is no longer delivered. Handoff-contract injection +
    * agent-call count prove the path ran; success via the persisted job status. */
   assert(strstr(g_last_agent_prompt, "delegate_result_v1") != NULL);
   assert(g_agent_calls == 1);
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_handoff_json_response\n");
}
static void test_direct_delegate_handoff_repair_attempt(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "not json";
   g_agent_repair_response =
       "{"
       "\"schema_version\":\"delegate_result_v1\","
       "\"status\":\"done\","
       "\"changed_files\":[],"
       "\"tests\":[{\"name\":\"unit-test-server-compute\",\"status\":\"passed\"}],"
       "\"supervisor_actions\":[],"
       "\"summary\":\"repair handoff ok\""
       "}";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the delegate malformed handoff repair test prompt");
   cJSON_AddTrueToObject(req, "handoff_json");
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
   /* (WP-B) async-only: handoff_* sync metadata gone; prompt + agent-call count
    * prove the malformed-handoff repair path ran; success via job status. */
   assert(strstr(g_last_agent_prompt, "Repair it now") != NULL);
   assert(g_agent_calls == 2);
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_handoff_repair_attempt\n");
}

static void test_direct_delegate_handoff_repair_failure_is_error(void)
{
   reset_last_response();
   /* Invalid delegate handoffs must stay errors after a failed repair attempt. */
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "not json";
   g_agent_repair_response = "still not json";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the delegate failed handoff repair test prompt");
   cJSON_AddTrueToObject(req, "handoff_json");
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
   /* (WP-B) async-only: the invalid-handoff error is persisted to the job row. */
   assert(g_agent_calls == 2);
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strstr(job.result, "invalid delegate handoff") != NULL);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_handoff_repair_failure_is_error\n");
}

static void test_direct_delegate_review_auto_tools_uses_tools(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "review completed with default tool execution";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "review");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the direct delegate review tool policy test prompt");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   /* (WP-B) async-only: read the persisted job, not the connection. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   assert(g_agent_run_calls == 0);
   assert(g_agent_tool_run_calls == 1);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_review_auto_tools_uses_tools\n");
}

static void test_direct_delegate_reviewer_alias_auto_tools_uses_tools(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "review alias completed with default tool execution";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "reviewer");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the direct delegate reviewer alias policy test");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   /* (WP-B) async-only: read the persisted job, not the connection. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   assert(g_agent_run_calls == 0);
   assert(g_agent_tool_run_calls == 1);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_reviewer_alias_auto_tools_uses_tools\n");
}

static void test_direct_delegate_explicit_tools_forces_tools(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "review completed with tool execution";
   g_budget_release_calls = 0;
   g_budget_last_grant = 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "review");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "run the direct delegate explicit tools policy test prompt");
   cJSON_AddTrueToObject(req, "tools");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   /* (WP-B) async-only: read the persisted job, not the connection. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   assert(g_agent_run_calls == 0);
   assert(g_agent_tool_run_calls == 1);
   assert(g_budget_release_calls == 1);
   assert(g_budget_last_grant == 1);
   assert(g_agent_seen_compute_override == 0);
   assert(g_agent_seen_budget_release_calls == 1);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_explicit_tools_forces_tools\n");
}

static void test_direct_delegate_one_turn_diagnose_suppresses_default_tools(void)
{
   reset_last_response();
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   int fds[2];
   assert(ctx != NULL && conn != NULL);
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   g_agent_response = "diagnose completed without implicit tools";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "diagnose");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the one turn diagnose final answer smoke test");
   cJSON_AddNumberToObject(req, "max_turns", 1);
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker && g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   close(fds[0]);
   assert(g_agent_run_calls == 1 && g_agent_tool_run_calls == 0);
   assert(g_last_agent_max_turns == 1);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
}

static void test_readonly_code_delegate_disables_write_enforce(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "readonly inspection complete";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "Read-only inspection only. Do not edit files or make changes.");
   cJSON_AddTrueToObject(req, "tools");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   /* (WP-B) async-only: read the persisted job, not the connection. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   assert(g_agent_tool_run_calls == 1);
   assert(g_last_write_enforce == 0);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_readonly_code_delegate_disables_write_enforce\n");
}
static void test_readonly_refactor_delegate_disables_write_enforce(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "readonly refactor inspection complete";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "refactor");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "Ownership: inspect only for now. Identify the safest split.");
   cJSON_AddTrueToObject(req, "tools");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   /* (WP-B) async-only: read the persisted job, not the connection. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   assert(g_agent_tool_run_calls == 1);
   assert(g_last_write_enforce == 0);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_readonly_refactor_delegate_disables_write_enforce\n");
}
static void test_direct_delegate_max_turns_override(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "delegate max turns override ok";
   g_last_agent_max_turns = -999;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the direct delegate max turns override test");
   cJSON_AddNumberToObject(req, "max_turns", 40);
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);
   assert(g_last_agent_max_turns == 40);
   /* (WP-B) async-only: the {job_id,"pending"} envelope is captured in
    * g_last_response; the override is asserted above via g_last_agent_max_turns. */
   assert(cJSON_IsObject(g_last_response));
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_max_turns_override\n");
}
