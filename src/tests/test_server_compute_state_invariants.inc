/* test_server_compute_state_invariants.inc: characterization net for the
 * delegate_worker run state machine — the env / thread-local / mailbox /
 * concurrency / cwd save-restore that wraps the provider run, plus the response
 * envelope shape. Split out of test_server_compute.c (same TU, after the impl
 * #includes and stubs) to keep that file under the 2000-line cap.
 *
 * These pin the *observable* behaviour the upcoming state-machine decomposition
 * must preserve: a delegate run must leave the caller's delegation context
 * (tl_delegation_depth / tl_parent_delegation_id, the AIMEE_DELEGATE_DEPTH /
 * AIMEE_PARENT_DELEGATION_ID / AIMEE_ACTIVE_TOOLSET env mirror), the mailbox,
 * the run cwd, the session override, and the concurrency slot exactly as it
 * found them — on both the success and failure paths. */

static const char *sci_getenv(const char *key)
{
   const char *v = getenv(key);
   return v ? v : "";
}

/* Drive one delegate end to end: handle_delegate captures the worker, we run it
 * synchronously, then parse the JSON it wrote to the connection fd. Returns the
 * parsed response (caller frees) or NULL. */
static cJSON *sci_drive_delegate(cJSON *req)
{
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[16384];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   close(fds[0]);
   free(conn);
   free(ctx);
   if (n <= 0)
      return NULL;
   buf[n] = '\0';
   /* (WP-B) async-only: handle_delegate returns the {job_id,"pending"} envelope;
    * the worker persists the real status/result to the job row. Return a
    * synthetic {status,message} object mapping the job back to the old vocabulary
    * (done->ok, cancelled->cancelled, else->error) so the invariant cases (which
    * mainly assert thread-local context restoration) still read a status. */
   cJSON *env = cJSON_Parse(buf);
   cJSON *jid = env ? cJSON_GetObjectItemCaseSensitive(env, "job_id") : NULL;
   cJSON *out = cJSON_CreateObject();
   if (cJSON_IsNumber(jid))
   {
      db1_agent_job_t job;
      if (db1_agent_job_get(jid->valueint, &job) == 0)
      {
         const char *st = strcmp(job.status, "done") == 0          ? "ok"
                          : strcmp(job.status, "cancelled") == 0   ? "cancelled"
                                                                    : "error";
         cJSON_AddStringToObject(out, "status", st);
         if (job.result && job.result[0])
            cJSON_AddStringToObject(out, "message", job.result);
         db1_agent_job_free(&job);
      }
   }
   cJSON_Delete(env);
   return out;
}

/* Establish a sentinel "outer delegate" context; assert the worker restores it. */
static void test_delegate_worker_restores_caller_context(void)
{
   reset_last_response();

   tl_delegation_depth = 1;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "outer-parent");
   platform_setenv("AIMEE_DELEGATE_DEPTH", "1");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "outer-parent");
   platform_setenv("AIMEE_ACTIVE_TOOLSET", "outer-toolset");
   run_cmd_set_cwd(NULL);
   tl_mailbox = NULL;

   g_agent_response = "did the thing";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute"); /* read-only: no worktree path */
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "characterize state restore");
   cJSON_AddStringToObject(req, "session_id", "inner-session");
   cJSON_AddStringToObject(req, "toolset", "inner-toolset");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "ok") == 0);
   cJSON_Delete(resp);

   /* Thread-local delegation context restored to the outer sentinels. */
   assert(tl_delegation_depth == 1);
   assert(strcmp(tl_parent_delegation_id, "outer-parent") == 0);
   /* Cross-process env mirror restored. */
   assert(strcmp(sci_getenv("AIMEE_DELEGATE_DEPTH"), "1") == 0);
   assert(strcmp(sci_getenv("AIMEE_PARENT_DELEGATION_ID"), "outer-parent") == 0);
   assert(strcmp(sci_getenv("AIMEE_ACTIVE_TOOLSET"), "outer-toolset") == 0);
   /* Mailbox released, run cwd cleared, session override cleared. */
   assert(tl_mailbox == NULL);
   assert(run_cmd_get_cwd() == NULL || run_cmd_get_cwd()[0] == '\0');
   {
      const char *s = session_id();
      assert(s == NULL || strcmp(s, "inner-session") != 0);
   }

   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';
   reset_last_response();
   printf("  PASS: test_delegate_worker_restores_caller_context\n");
}

/* The per-model concurrency slot acquired for the run must be released exactly once. */
static void test_delegate_worker_balances_concurrency_slot(void)
{
   reset_last_response();
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';

   g_agent_response = "ok";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "concurrency balance");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "ok") == 0);
   cJSON_Delete(resp);

   assert(g_concurrency_acquire_calls == 1);
   assert(g_concurrency_release_calls == g_concurrency_acquire_calls);

   reset_last_response();
   printf("  PASS: test_delegate_worker_balances_concurrency_slot\n");
}

/* The success response carries the canonical delegate envelope fields. */
static void test_delegate_worker_ok_response_shape(void)
{
   reset_last_response();
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';

   g_agent_response = "the delegate result text";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "response shape");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(cJSON_IsString(cJSON_GetObjectItem(resp, "delegation_id")));
   assert(cJSON_GetObjectItem(resp, "delegation_id")->valuestring[0] != '\0');
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "ok") == 0);
   assert(strcmp(cJSON_GetObjectItem(resp, "response")->valuestring, "the delegate result text") ==
          0);
   assert(cJSON_HasObjectItem(resp, "turns"));
   assert(cJSON_HasObjectItem(resp, "tool_calls"));
   assert(cJSON_HasObjectItem(resp, "agent"));
   assert(cJSON_HasObjectItem(resp, "prompt_tokens"));
   assert(cJSON_HasObjectItem(resp, "completion_tokens"));
   cJSON_Delete(resp);

   reset_last_response();
   printf("  PASS: test_delegate_worker_ok_response_shape\n");
}

/* The same context-restore invariants must hold when the run fails (rc != 0). */
static void test_delegate_worker_restores_state_on_error(void)
{
   reset_last_response();

   tl_delegation_depth = 1;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "err-parent");
   platform_setenv("AIMEE_DELEGATE_DEPTH", "1");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "err-parent");
   platform_setenv("AIMEE_ACTIVE_TOOLSET", "err-toolset");
   run_cmd_set_cwd(NULL);
   tl_mailbox = NULL;

   g_agent_run_rc = -1; /* force the provider run to fail */
   g_agent_response = NULL;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "error-path restore");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "error") == 0);
   cJSON_Delete(resp);

   /* Restore invariants must hold on the failure path too. */
   assert(tl_delegation_depth == 1);
   assert(strcmp(tl_parent_delegation_id, "err-parent") == 0);
   assert(strcmp(sci_getenv("AIMEE_DELEGATE_DEPTH"), "1") == 0);
   assert(strcmp(sci_getenv("AIMEE_PARENT_DELEGATION_ID"), "err-parent") == 0);
   assert(strcmp(sci_getenv("AIMEE_ACTIVE_TOOLSET"), "err-toolset") == 0);
   assert(tl_mailbox == NULL);
   assert(run_cmd_get_cwd() == NULL || run_cmd_get_cwd()[0] == '\0');
   assert(g_concurrency_release_calls == g_concurrency_acquire_calls);

   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';
   reset_last_response();
   printf("  PASS: test_delegate_worker_restores_state_on_error\n");
}

/* The session override must be active *during* the provider run so the agent's
 * tool calls resolve to the delegating session. */
static void test_delegate_worker_sets_session_override_during_run(void)
{
   reset_last_response();
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';

   g_agent_response = "ok";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "session-during-run");
   cJSON_AddStringToObject(req, "session_id", "run-session");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "ok") == 0);
   cJSON_Delete(resp);

   assert(g_agent_calls == 1);
   assert(strcmp(g_session_during_run, "run-session") == 0);

   reset_last_response();
   printf("  PASS: test_delegate_worker_sets_session_override_during_run\n");
}

/* Concurrency cancelled before a slot is granted: the worker must respond
 * "cancelled", never run the provider, and unwind the caller context — the
 * early-return path that duplicates the normal teardown. */
static void test_delegate_worker_concurrency_cancelled_restores(void)
{
   reset_last_response();
   tl_delegation_depth = 1;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "cx-parent");
   platform_setenv("AIMEE_DELEGATE_DEPTH", "1");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "cx-parent");
   run_cmd_set_cwd(NULL);
   tl_mailbox = NULL;

   g_concurrency_force_status = CONCURRENCY_ACQUIRE_CANCELLED;
   g_agent_response = "should not run";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "concurrency cancelled");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "cancelled") == 0);
   cJSON_Delete(resp);

   /* The provider must never have run, and no slot was held. */
   assert(g_agent_calls == 0);
   assert(g_concurrency_acquire_calls == 0);
   assert(g_concurrency_release_calls == 0);
   /* Caller context unwound on the cancelled path. */
   assert(tl_delegation_depth == 1);
   assert(strcmp(tl_parent_delegation_id, "cx-parent") == 0);
   assert(strcmp(sci_getenv("AIMEE_DELEGATE_DEPTH"), "1") == 0);
   assert(strcmp(sci_getenv("AIMEE_PARENT_DELEGATION_ID"), "cx-parent") == 0);
   assert(tl_mailbox == NULL);
   assert(run_cmd_get_cwd() == NULL || run_cmd_get_cwd()[0] == '\0');

   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';
   reset_last_response();
   printf("  PASS: test_delegate_worker_concurrency_cancelled_restores\n");
}

/* Concurrency queue full: same early-return teardown, but the response is an
 * error (queue full is a rejection, not a cancellation). */
static void test_delegate_worker_concurrency_queue_full_errors(void)
{
   reset_last_response();
   tl_delegation_depth = 1;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "qf-parent");
   platform_setenv("AIMEE_DELEGATE_DEPTH", "1");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "qf-parent");
   tl_mailbox = NULL;

   g_concurrency_force_status = CONCURRENCY_ACQUIRE_QUEUE_FULL;
   g_agent_response = "should not run";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "concurrency queue full");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "error") == 0);
   assert(strstr(cJSON_GetObjectItem(resp, "message")->valuestring, "queue full") != NULL);
   cJSON_Delete(resp);

   assert(g_agent_calls == 0);
   assert(g_concurrency_release_calls == g_concurrency_acquire_calls);
   assert(tl_delegation_depth == 1);
   assert(strcmp(tl_parent_delegation_id, "qf-parent") == 0);
   assert(strcmp(sci_getenv("AIMEE_DELEGATE_DEPTH"), "1") == 0);
   assert(tl_mailbox == NULL);

   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';
   reset_last_response();
   printf("  PASS: test_delegate_worker_concurrency_queue_full_errors\n");
}
