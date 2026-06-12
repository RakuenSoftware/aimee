#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "aimee.h"
#include "db.h"
#include "db_schema.h"
#include "db1.h"
#include "server.h"
#include <sqlite3.h>
/* Private to src/db1/, but test_server_compute reads delegation_spawns and
 * delegation_messages directly to assert state written by the shims. */
extern sqlite3 *db1_conn(void);
int server_session_pool_submit(server_ctx_t *ctx, const char *session_id, void (*fn)(void *),
                               void *arg, int *thread_count_out)
{
   (void)session_id;
   if (thread_count_out)
      *thread_count_out =
          ctx && ctx->session_threads > 0 ? ctx->session_threads : CONFIG_DEFAULT_SESSION_THREADS;
   if (fn)
      fn(arg);
   return 0;
}
#include "../server/server_compute.c"
#include "../server/server_compute_async.c"
static cJSON *g_last_response = NULL;
static char g_last_error[256];
static void (*g_submitted_fn)(void *) = NULL;
static void *g_submitted_arg = NULL;
static const char *g_agent_response = NULL;
static const char *g_agent_repair_response = NULL;
static int g_agent_calls = 0;
static int g_agent_run_calls = 0;
static int g_agent_tool_run_calls = 0, g_config_tools_enabled = 1;
static int g_last_agent_max_turns = -999;
static int g_last_write_enforce = -999;
static int g_budget_acquire_calls = 0;
static int g_budget_release_calls = 0;
static int g_budget_last_grant = 0;
static int g_agent_run_seen_compute_override = -999;
static int g_agent_seen_compute_override = -999;
static int g_agent_seen_budget_release_calls = -999;
static int g_last_concurrency_priority = -999;
static int g_concurrency_acquire_calls = 0;
static int g_concurrency_release_calls = 0;
static int g_concurrency_force_status =
    0; /* CONCURRENCY_ACQUIRE_OK; non-OK -> acquire returns NULL */
static int g_agent_run_rc = 0;
static char g_session_during_run[128]; /* session_id() observed inside the provider run */
static char g_last_agent_prompt[4096];
static char g_last_system_prompt[4096];
static int g_git_repo_root_rc = -1;
static char g_git_repo_root_value[MAX_PATH_LEN];
static int g_worktree_create_rc = -1;
static int g_worktree_create_calls = 0;
static int g_worktree_sibling_path_rc = -1;
static char g_worktree_sibling_path_value[MAX_PATH_LEN];
static int g_worktree_apply_calls = 0;
static int g_delegate_apply_calls = 0;
static int g_delegate_apply_rc = 1;
static char g_last_apply_src[MAX_PATH_LEN], g_last_apply_dst[MAX_PATH_LEN];
void chat_stream_worker(void *arg)
{
   (void)arg;
}
static void fake_agent_fill_response(const char *prompt, agent_result_t *result)
{
   g_agent_calls++;
   /* Capture the session override active during the run: delegate_worker must
    * set it before invoking the provider so the agent's tool calls resolve to
    * the delegating session. */
   {
      const char *s = session_id();
      snprintf(g_session_during_run, sizeof(g_session_during_run), "%s", s ? s : "");
   }
   if (prompt)
      snprintf(g_last_agent_prompt, sizeof(g_last_agent_prompt), "%s", prompt);
   const char *response =
       (g_agent_calls > 1 && g_agent_repair_response) ? g_agent_repair_response : g_agent_response;
   if (response)
      result->response = strdup(response);
}
static void *waiter_thread(void *arg)
{
   delegation_mailbox_t *mb = (delegation_mailbox_t *)arg;
   char *out = malloc(64);
   assert(out != NULL);
   int rc = mailbox_wait(mb, out, 64, 2);
   assert(rc == 0);
   return out;
}
typedef struct
{
   int fd;
   size_t total;
} drain_ctx_t;

static void *drain_pipe_thread(void *arg)
{
   drain_ctx_t *ctx = (drain_ctx_t *)arg;
   char buf[4096];
   usleep(50000);
   while (ctx->total > 0)
   {
      ssize_t n = read(ctx->fd, buf, sizeof(buf));
      if (n <= 0)
         break;
      ctx->total -= (size_t)n;
   }
   return NULL;
}

void agent_set_request_codex_creds(const char *token, const char *account_id)
{
   (void)token;
   (void)account_id;
}
void agent_set_request_session(const char *session_id)
{
   (void)session_id;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->agent_count = 1;
   snprintf(cfg->agents[0].name, sizeof(cfg->agents[0].name), "test-agent");
   snprintf(cfg->agents[0].model, sizeof(cfg->agents[0].model), "test-model");
   snprintf(cfg->agents[0].provider, sizeof(cfg->agents[0].provider), "openai");
   cfg->agents[0].enabled = 1;
   cfg->agents[0].tools_enabled = g_config_tools_enabled;
   cfg->agents[0].max_tokens = 4096;
   cfg->agents[0].max_turns = -1;
   return 0;
}

const char *agent_config_path(void)
{
   return "/tmp/aimee-test-agents.json";
}

void delegate_apply_max_turns_policy(agent_config_t *cfg, const char *role, int max_turns)
{
   (void)role;
   if (!cfg || max_turns < 0)
      return;
   for (int i = 0; i < cfg->agent_count; i++)
      cfg->agents[i].max_turns = max_turns;
}
char *role_template_build(const char *project_root, const char *role, const char *task,
                          const char *context)
{
   (void)project_root;
   (void)role;
   (void)task;
   (void)context;
   return NULL;
}

void agent_http_init(void)
{
}

int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt, const char *prompt,
              int max_tokens, agent_result_t *result)
{
   (void)role;
   (void)prompt;
   (void)max_tokens;
   if (system_prompt)
      snprintf(g_last_system_prompt, sizeof(g_last_system_prompt), "%s", system_prompt);
   memset(result, 0, sizeof(*result));
   g_agent_run_calls++;
   g_agent_run_seen_compute_override = g_aimee_compute_threads_override;
   g_last_agent_max_turns = cfg->agent_count > 0 ? cfg->agents[0].max_turns : -999;
   g_agent_seen_compute_override = g_aimee_compute_threads_override;
   g_agent_seen_budget_release_calls = g_budget_release_calls;
   fake_agent_fill_response(prompt, result);
   if (g_agent_run_rc != 0)
      snprintf(result->error, sizeof(result->error), "stubbed agent failure");
   return g_agent_run_rc;
}

int agent_run_with_tools(agent_config_t *cfg, const char *role, const char *system_prompt,
                         const char *prompt, int max_tokens, agent_result_t *result)
{
   (void)role;
   (void)prompt;
   (void)max_tokens;
   if (system_prompt)
      snprintf(g_last_system_prompt, sizeof(g_last_system_prompt), "%s", system_prompt);
   memset(result, 0, sizeof(*result));
   g_agent_tool_run_calls++;
   g_agent_run_seen_compute_override = g_aimee_compute_threads_override;
   g_last_agent_max_turns = cfg->agent_count > 0 ? cfg->agents[0].max_turns : -999;
   g_agent_seen_compute_override = g_aimee_compute_threads_override;
   g_agent_seen_budget_release_calls = g_budget_release_calls;
   fake_agent_fill_response(prompt, result);
   if (g_agent_run_rc != 0)
      snprintf(result->error, sizeof(result->error), "stubbed agent failure");
   return g_agent_run_rc;
}

int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const char *system_prompt, const char *prompt,
                                       int max_tokens, int enforce_writes, agent_result_t *result)
{
   g_last_write_enforce = enforce_writes;
   return agent_run_with_tools(cfg, role, system_prompt, prompt, max_tokens, result);
}

char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms)
{
   (void)name;
   (void)arguments_json;
   (void)timeout_ms;
   return strdup("ok");
}

void agent_tools_parent_write_guard_set(const char *r, const char *w)
{
   (void)r;
   (void)w;
}
void agent_tools_parent_write_guard_clear(void)
{
}

int pre_tool_check(const char *tool_name, const char *tool_input, session_state_t *state,
                   const char *guardrail_mode, const char *cwd, char *msg, size_t msg_len)
{
   (void)tool_name;
   (void)tool_input;
   (void)state;
   (void)guardrail_mode;
   (void)cwd;
   if (msg_len > 0)
      msg[0] = '\0';
   return 0;
}

void session_state_load(session_state_t *state, const char *sid)
{
   (void)sid;
   memset(state, 0, sizeof(*state));
}
void session_state_save(const session_state_t *state, const char *sid)
{
   (void)state;
   (void)sid;
}

int server_compute_budget_acquire(server_ctx_t *ctx)
{
   (void)ctx;
   g_budget_acquire_calls++;
   return 1;
}

void server_compute_budget_release(server_ctx_t *ctx, int granted)
{
   (void)ctx;
   g_budget_release_calls++;
   g_budget_last_grant = granted;
}

void agent_set_durable_job(int job_id)
{
   (void)job_id;
}
agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   (void)role;
   return cfg && cfg->agent_count > 0 ? &cfg->agents[0] : NULL;
}
agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role, const config_t *sys_cfg,
                               unsigned required_caps, int min_context)
{
   (void)sys_cfg;
   (void)required_caps;
   (void)min_context;
   return agent_route(cfg, role);
}
int agent_is_claude_cli(const agent_t *agent)
{
   (void)agent;
   return 0; /* stub: the claude-cli delegate gate is exercised in test_agent */
}
agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   if (!cfg || !name)
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   return NULL;
}

int agent_has_role(const agent_t *agent, const char *role)
{
   if (!agent || !role)
      return 0;
   for (int i = 0; i < agent->role_count; i++)
      if (strcmp(agent->roles[i], role) == 0)
         return 1;
   return 0;
}

int agent_is_exec_role(const agent_t *agent, const char *role)
{
   (void)agent;
   static const char *defaults[] = {"deploy", "validate", "test",     "diagnose", "execute",
                                    "review", "code",     "refactor", "draft",    "implement"};
   if (!role)
      return 0;
   for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
      if (strcmp(defaults[i], role) == 0)
         return 1;
   return 0;
}

int agent_is_available_for_routing(const agent_t *agent)
{
   (void)agent;
   return 1;
}
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier)
{
   (void)role;
   if (!cfg)
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
      if (cfg->agents[i].cost_tier == tier)
         return &cfg->agents[i];
   return NULL;
}
char *resolve_file_references(const char *prompt, const char *project_root)
{
   (void)prompt;
   (void)project_root;
   return NULL;
}
int delegate_token_budget_load(const char *project_root, const char *role)
{
   (void)project_root;
   (void)role;
   return 20000;
}
char *delegate_prompt_limit(const char *prompt, int token_budget)
{
   (void)token_budget;
   return NULL;
}

char *prompt_prepend_principles(aimee_mode_t mode, const char *base_prompt)
{
   (void)mode;
   const char *base = base_prompt ? base_prompt : "";
   const char *principles =
       "# Code Principles\n"
       "- Prefer composition over inheritance and small focused modules over deep "
       "hierarchies.\n";
   size_t plen = strlen(principles);
   size_t blen = strlen(base);
   size_t prefix_len = strncmp(base, principles, plen) == 0 ? 0 : plen;
   char *out = malloc(prefix_len + blen + 1);
   assert(out != NULL);
   if (prefix_len)
      memcpy(out, principles, prefix_len);
   memcpy(out + prefix_len, base, blen + 1);
   return out;
}

/* Persona composition stub: this test does not exercise per-delegate personas,
 * so fall back to the engineer principles regardless of the requested name. */
char *persona_compose_delegate_prompt(const char *name, const char *cwd, const char *base_prompt)
{
   (void)name;
   (void)cwd;
   return prompt_prepend_principles(AIMEE_MODE_ENGINEER, base_prompt);
}

/* Delegation admission stub: this test does not exercise persona/delegate-policy
 * behavior, so admit every (non-empty) delegation request. */
const char *server_http_delegate_block(const char *session_id, const char *role, const char *prompt,
                                       char *buf, size_t n)
{
   (void)session_id;
   (void)role;
   (void)prompt;
   (void)buf;
   (void)n;
   return NULL;
}

#include "test_server_compute_workspace_stubs.inc"

/* delegate_routing reaches the kb DB2 bandit over kb_client, which this unit test
 * does not link. Stub it as "sampling disabled" so delegate_worker falls back to
 * default routing (and never closes a decision). */
int kb_client_bandit_sample(const char *decision_point, const char *const *arms, int n_arms,
                            char *arm_out, size_t arm_out_len, char *decision_id_out,
                            size_t decision_id_out_len)
{
   (void)decision_point;
   (void)arms;
   (void)n_arms;
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   if (decision_id_out && decision_id_out_len)
      decision_id_out[0] = '\0';
   return -1;
}
int kb_client_bandit_close(const char *decision_point, const char *decision_id, const char *arm_id,
                           double reward)
{
   (void)decision_point;
   (void)decision_id;
   (void)arm_id;
   (void)reward;
   return 0;
}

void concurrency_mgr_init(concurrency_mgr_t *mgr, int default_limit,
                          const concurrency_entry_t *per_model, int per_model_count,
                          const concurrency_entry_t *per_provider, int per_provider_count)
{
   (void)mgr;
   (void)default_limit;
   (void)per_model;
   (void)per_model_count;
   (void)per_provider;
   (void)per_provider_count;
}

void concurrency_mgr_update_limits(concurrency_mgr_t *mgr, int default_limit,
                                   const concurrency_entry_t *per_model, int per_model_count,
                                   const concurrency_entry_t *per_provider, int per_provider_count)
{
   (void)mgr;
   (void)default_limit;
   (void)per_model;
   (void)per_model_count;
   (void)per_provider;
   (void)per_provider_count;
}

concurrency_slot_t *concurrency_acquire_cancellable(concurrency_mgr_t *mgr, const char *model,
                                                    const char *provider,
                                                    concurrency_cancel_fn_t cancel_fn,
                                                    const char *cancel_ctx)
{
   return concurrency_acquire_priority_cancellable(
       mgr, model, provider, CONCURRENCY_PRIORITY_INTERACTIVE, cancel_fn, cancel_ctx);
}

concurrency_slot_t *concurrency_acquire_priority_cancellable(concurrency_mgr_t *mgr,
                                                             const char *model,
                                                             const char *provider, int priority,
                                                             concurrency_cancel_fn_t cancel_fn,
                                                             const char *cancel_ctx)
{
   return concurrency_acquire_priority_cancellable_status(mgr, model, provider, priority, cancel_fn,
                                                          cancel_ctx, NULL);
}

concurrency_slot_t *concurrency_acquire_priority_cancellable_status(
    concurrency_mgr_t *mgr, const char *model, const char *provider, int priority,
    concurrency_cancel_fn_t cancel_fn, const char *cancel_ctx,
    concurrency_acquire_status_t *status_out)
{
   return concurrency_acquire_priority_owner_cancellable_status(
       mgr, model, provider, priority, NULL, cancel_fn, cancel_ctx, status_out);
}

concurrency_slot_t *concurrency_acquire_priority_owner_cancellable_status(
    concurrency_mgr_t *mgr, const char *model, const char *provider, int priority,
    const char *owner_id, concurrency_cancel_fn_t cancel_fn, const char *cancel_ctx,
    concurrency_acquire_status_t *status_out)
{
   static concurrency_slot_t slot;
   (void)mgr;
   (void)model;
   (void)provider;
   (void)owner_id;
   (void)cancel_fn;
   (void)cancel_ctx;
   g_last_concurrency_priority = priority;
   /* Forced non-OK status: no slot granted (cancelled / queue-full path). */
   if (g_concurrency_force_status != CONCURRENCY_ACQUIRE_OK)
   {
      if (status_out)
         *status_out = (concurrency_acquire_status_t)g_concurrency_force_status;
      return NULL;
   }
   g_concurrency_acquire_calls++;
   if (status_out)
      *status_out = CONCURRENCY_ACQUIRE_OK;
   return &slot;
}

void concurrency_release(concurrency_slot_t *slot)
{
   concurrency_release_owner(slot, NULL);
}

void concurrency_release_owner(concurrency_slot_t *slot, const char *owner_id)
{
   (void)slot;
   (void)owner_id;
   g_concurrency_release_calls++;
}

int concurrency_preempt_candidate_for_key(concurrency_mgr_t *mgr, const char *model,
                                          const char *provider, int requester_priority,
                                          int single_slot_only, char *owner_out,
                                          size_t owner_out_len, int *priority_out)
{
   (void)mgr;
   (void)model;
   (void)provider;
   (void)requester_priority;
   (void)single_slot_only;
   (void)owner_out;
   (void)owner_out_len;
   (void)priority_out;
   return 0;
}

int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg)
{
   (void)pool;
   g_submitted_fn = fn;
   g_submitted_arg = arg;
   return 0;
}

/* The real compute_pool slot-tracking helpers live in compute_pool.c, which
 * we don't link in (the test stubs compute_pool_submit above). delegate_worker
 * and tool_execute_worker call set_job/clear_job to publish their identity to
 * `aimee workers`; in the test environment we just no-op. */
void compute_pool_set_job(pool_job_kind_t kind, const char *descriptor_fmt, ...)
{
   (void)kind;
   (void)descriptor_fmt;
}

void compute_pool_clear_job(void)
{
}

/* Tests want delegate_worker captured rather than launched on a fresh thread.
 * Override the production pthread/pool dispatch with a synchronous capture. */
static int test_delegate_dispatch_stub(compute_ctx_t *cctx)
{
   g_submitted_fn = delegate_worker;
   g_submitted_arg = cctx;
   return 0;
}

static int test_chat_dispatch_stub(compute_ctx_t *cctx)
{
   g_submitted_fn = chat_stream_worker;
   g_submitted_arg = cctx;
   return 0;
}

static int test_tool_dispatch_stub(compute_ctx_t *cctx)
{
   g_submitted_fn = tool_execute_worker;
   g_submitted_arg = cctx;
   return 0;
}

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   cJSON_Delete(g_last_response);
   g_last_response = cJSON_Duplicate(resp, 1);
   g_last_error[0] = '\0';
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(g_last_error, sizeof(g_last_error), "%s", message ? message : "");
   return 0;
}

static void reset_last_response(void)
{
   cJSON_Delete(g_last_response);
   g_last_response = NULL;
   g_last_error[0] = '\0';
   g_submitted_fn = NULL;
   g_submitted_arg = NULL;
   g_agent_response = NULL;
   g_agent_repair_response = NULL;
   g_agent_calls = 0;
   g_agent_run_calls = 0;
   g_agent_tool_run_calls = 0, g_config_tools_enabled = 1;
   g_last_write_enforce = -999;
   g_agent_seen_compute_override = -999;
   g_agent_seen_budget_release_calls = -999;
   g_last_agent_prompt[0] = '\0';
   g_last_system_prompt[0] = '\0';
   g_git_repo_root_rc = -1;
   g_git_repo_root_value[0] = '\0';
   g_worktree_create_rc = -1;
   g_worktree_create_calls = 0;
   g_worktree_sibling_path_rc = -1;
   g_worktree_sibling_path_value[0] = g_last_apply_src[0] = g_last_apply_dst[0] = '\0';
   g_worktree_apply_calls = 0;
   g_delegate_apply_calls = 0;
   g_delegate_apply_rc = 1;
   g_budget_acquire_calls = 0;
   g_budget_release_calls = 0;
   g_budget_last_grant = 0;
   g_agent_run_seen_compute_override = -999;
   g_last_concurrency_priority = -999;
   g_concurrency_acquire_calls = 0;
   g_concurrency_release_calls = 0;
   g_concurrency_force_status = CONCURRENCY_ACQUIRE_OK;
   g_agent_run_rc = 0;
   g_session_during_run[0] = '\0';
   g_delegate_dispatch_override = test_delegate_dispatch_stub;
   g_chat_dispatch_override = NULL;
   g_tool_dispatch_override = NULL;
}

static void test_mailbox_lifecycle(void)
{
   delegation_mailbox_t *mb = mailbox_acquire("deleg-1");
   assert(mb != NULL);
   assert(mailbox_find("deleg-1") == mb);
   mailbox_release(mb);
   assert(mailbox_find("deleg-1") == NULL);
}

static void test_reply_wakeup(void)
{
   delegation_mailbox_t *mb = mailbox_acquire("deleg-2");
   assert(mb != NULL);
   pthread_t thread;
   assert(pthread_create(&thread, NULL, waiter_thread, mb) == 0);
   usleep(20000);
   mailbox_reply(mb, "parent reply");
   char *out = NULL;
   assert(pthread_join(thread, (void **)&out) == 0);
   assert(strcmp(out, "parent reply") == 0);
   free(out);
   mailbox_release(mb);
}

static void test_timeout_and_no_mailbox(void)
{
   delegation_mailbox_t *mb = mailbox_acquire("deleg-timeout");
   assert(mb != NULL);
   char reply[32];
   assert(mailbox_wait(mb, reply, sizeof(reply), 0) != 0);
   mailbox_release(mb);

   tl_mailbox = NULL;
   assert(delegation_request_input("question?") == NULL);
}

static void test_message_recording(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);

   delegation_mailbox_t *mb = mailbox_acquire("deleg-db");
   assert(mb != NULL);
   tl_mailbox = mb;

   assert(db1_delegation_message_record("deleg-db", "delegate_to_parent", "question") == 0);
   assert(db1_delegation_message_record("deleg-db", "parent_to_delegate", "answer") == 0);

   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db1_conn(),
                             "SELECT direction, content FROM delegation_messages "
                             "WHERE delegation_id = ? ORDER BY id",
                             -1, &stmt, NULL) == SQLITE_OK);
   assert(stmt != NULL);
   sqlite3_bind_text(stmt, 1, "deleg-db", -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "delegate_to_parent") == 0);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "question") == 0);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "parent_to_delegate") == 0);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "answer") == 0);
   sqlite3_finalize(stmt);

   tl_mailbox = NULL;
   mailbox_release(mb);
   db1_stmt_cache_clear();
   sqlite3_close(db);
}

static void test_write_all_handles_eagain(void)
{
   int fds[2];
   assert(pipe(fds) == 0);
   int flags = fcntl(fds[1], F_GETFL, 0);
   assert(flags >= 0);
   assert(fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);

   char filler[4096];
   memset(filler, 'x', sizeof(filler));
   while (write(fds[1], filler, sizeof(filler)) > 0)
   {
   }
   assert(errno == EAGAIN || errno == EWOULDBLOCK);

   char payload[16384];
   memset(payload, 'y', sizeof(payload));
   drain_ctx_t ctx = {.fd = fds[0], .total = sizeof(payload) + sizeof(filler)};
   pthread_t drain_thread;
   assert(pthread_create(&drain_thread, NULL, drain_pipe_thread, &ctx) == 0);
   assert(write_all(fds[1], payload, sizeof(payload)) == 0);
   close(fds[1]);
   pthread_join(drain_thread, NULL);
   close(fds[0]);
}

static void test_compute_ctx_release_budget_clears_grant_and_override(void)
{
   g_budget_release_calls = 0;
   g_budget_last_grant = 0;
   g_aimee_compute_threads_override = 1;

   compute_ctx_t cctx;
   memset(&cctx, 0, sizeof(cctx));
   cctx.compute_grant = 1;

   compute_ctx_release_budget(&cctx);
   assert(cctx.compute_grant == 0);
   assert(g_aimee_compute_threads_override == 0);
   assert(g_budget_release_calls == 1);
   assert(g_budget_last_grant == 1);

   compute_ctx_release_budget(&cctx);
   assert(g_budget_release_calls == 1);
}

static void test_depth_limit_enforcement(void)
{
   int req_parent_depth = 0;
   tl_delegation_depth = 0;
   int parent_depth =
       tl_delegation_depth > req_parent_depth ? tl_delegation_depth : req_parent_depth;
   assert(parent_depth == 0);

   tl_delegation_depth = 1;
   parent_depth = tl_delegation_depth > req_parent_depth ? tl_delegation_depth : req_parent_depth;
   assert(parent_depth > 0);

   tl_delegation_depth = 0;
   req_parent_depth = 1;
   parent_depth = tl_delegation_depth > req_parent_depth ? tl_delegation_depth : req_parent_depth;
   assert(parent_depth > 0);

   tl_delegation_depth = 0;
}

/* Total spawns tracking: keep legacy session totals for diagnostics, and count
 * nested descendants separately for the spawn limiter. Completed descendants
 * still count, because a runaway nested chain can finish between spawns. */
static void test_spawn_count_total_tracking(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);

   assert(db1_delegation_spawn_count_total("sess-a") == 0);

   assert(db1_delegation_spawn_record("deleg-a1", NULL, "sess-a", 1, "code") == 0);
   assert(db1_delegation_spawn_record("deleg-a2", "deleg-a1", "sess-a", 2, "review") == 0);
   assert(db1_delegation_spawn_record("deleg-a3", "deleg-a2", "sess-a", 3, "review") == 0);
   assert(db1_delegation_spawn_count_total("sess-a") == 3);
   assert(db1_delegation_spawn_count_descendants("deleg-a1") == 2);

   char root[64];
   assert(db1_delegation_spawn_find_root("deleg-a3", root, sizeof(root)) == 0);
   assert(strcmp(root, "deleg-a1") == 0);

   /* Completing a spawn must NOT reduce the total — runaway chains that
    * complete between spawns still count against the budget. */
   assert(db1_delegation_spawn_complete("deleg-a1") == 0);
   assert(db1_delegation_spawn_count_total("sess-a") == 3);
   assert(db1_delegation_spawn_count_descendants("deleg-a1") == 2);
   assert(db1_delegation_spawn_complete("deleg-a2") == 0);
   assert(db1_delegation_spawn_count_total("sess-a") == 3);

   /* Independent sessions keep independent totals. */
   assert(db1_delegation_spawn_record("deleg-b1", NULL, "sess-b", 1, "code") == 0);
   assert(db1_delegation_spawn_count_total("sess-a") == 3);
   assert(db1_delegation_spawn_count_total("sess-b") == 1);
   assert(db1_delegation_spawn_count_descendants("deleg-b1") == 0);

   db1_stmt_cache_clear();
   sqlite3_close(db);
}

static void test_spawn_record_marks_running(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);

   assert(db1_delegation_spawn_record("deleg-running", NULL, "sess-running", 1, "summarize") == 0);

   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db1_conn(),
                             "SELECT status FROM delegation_spawns WHERE delegation_id = ?", -1,
                             &stmt, NULL) == SQLITE_OK);
   assert(stmt != NULL);
   sqlite3_bind_text(stmt, 1, "deleg-running", -1, SQLITE_TRANSIENT);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "running") == 0);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear();
   sqlite3_close(db);
}

/* Fill one root delegate with completed descendants up to the limit and confirm
 * the descendant-based check would reject the next nested spawn. Top-level
 * delegates in the same operator session are deliberately not capped by this
 * limiter, so long delegate-heavy sessions do not exhaust themselves. */
static void test_spawn_limit_descendants_blocks_completed_chain(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);

   int max_spawns = CONFIG_DEFAULT_MAX_DELEGATION_SPAWNS;
   assert(db1_delegation_spawn_record("root-deleg", NULL, "chain-session", 1, "code") == 0);

   char deleg_id[64];
   for (int i = 0; i < max_spawns; i++)
   {
      snprintf(deleg_id, sizeof(deleg_id), "deleg-seq-%d", i);
      assert(db1_delegation_spawn_record(deleg_id, "root-deleg", "chain-session", 2, "code") == 0);
      assert(db1_delegation_spawn_complete(deleg_id) == 0);
   }

   /* All nested delegates have finished — the 51st nested spawn must still be
    * rejected because the root descendant count is at the limit. */
   int total = db1_delegation_spawn_count_descendants("root-deleg");
   assert(total == max_spawns);
   assert(total >= max_spawns);

   /* Many completed first-level delegates should not consume root-delegate
    * descendant budget. */
   for (int i = 0; i < max_spawns; i++)
   {
      snprintf(deleg_id, sizeof(deleg_id), "top-level-%d", i);
      assert(db1_delegation_spawn_record(deleg_id, NULL, "operator-session", 1, "review") == 0);
      assert(db1_delegation_spawn_complete(deleg_id) == 0);
      assert(db1_delegation_spawn_count_descendants(deleg_id) == 0);
   }

   /* Custom limit path: half the default should also cap correctly. */
   int custom_limit = max_spawns / 2;
   assert(db1_delegation_spawn_record("other-root", NULL, "other-session", 1, "code") == 0);
   int other_total = db1_delegation_spawn_count_descendants("other-root");
   assert(other_total == 0);
   for (int i = 0; i < custom_limit; i++)
   {
      snprintf(deleg_id, sizeof(deleg_id), "other-%d", i);
      assert(db1_delegation_spawn_record(deleg_id, "other-root", "other-session", 2, "code") == 0);
      assert(db1_delegation_spawn_complete(deleg_id) == 0);
   }
   other_total = db1_delegation_spawn_count_descendants("other-root");
   assert(other_total == custom_limit);
   assert(other_total >= custom_limit);

   db1_stmt_cache_clear();
   sqlite3_close(db);
}

static void test_depth_context_save_restore(void)
{
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';

   char saved_parent[64];
   int saved_depth = tl_delegation_depth;
   snprintf(saved_parent, sizeof(saved_parent), "%s", tl_parent_delegation_id);
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "deleg-A");
   tl_delegation_depth = 1;

   char saved_parent2[64];
   int saved_depth2 = tl_delegation_depth;
   snprintf(saved_parent2, sizeof(saved_parent2), "%s", tl_parent_delegation_id);
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "deleg-B");
   tl_delegation_depth = 2;

   assert(tl_delegation_depth == 2);
   assert(strcmp(tl_parent_delegation_id, "deleg-B") == 0);

   tl_delegation_depth = saved_depth2;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "%s", saved_parent2);
   assert(tl_delegation_depth == 1);
   assert(strcmp(tl_parent_delegation_id, "deleg-A") == 0);

   tl_delegation_depth = saved_depth;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "%s", saved_parent);
   assert(tl_delegation_depth == 0);
   assert(tl_parent_delegation_id[0] == '\0');
}

static void test_delegation_augment_error(void)
{
   char buf[2048];

   /* Known patterns get a Fix hint appended. */
   delegation_augment_error("missing prompt", buf, sizeof(buf));
   assert(strstr(buf, "missing prompt") != NULL);
   assert(strstr(buf, "Fix:") != NULL);

   delegation_augment_error("prompt too short (5 chars)", buf, sizeof(buf));
   assert(strstr(buf, "Fix:") != NULL);
   assert(strstr(buf, "20 char") != NULL);

   delegation_augment_error("no agent available for role 'wat'", buf, sizeof(buf));
   assert(strstr(buf, "Fix:") != NULL);
   assert(strstr(buf, "agents.json") != NULL);

   delegation_augment_error("missing delegation_id or content", buf, sizeof(buf));
   assert(strstr(buf, "Fix:") != NULL);

   delegation_augment_error("delegation depth limit exceeded (3/3)", buf, sizeof(buf));
   assert(strstr(buf, "Fix:") != NULL);
   assert(strstr(buf, "max_delegation_depth") != NULL);

   delegation_augment_error("delegation spawn limit exceeded (51/50 total delegates for session)",
                            buf, sizeof(buf));
   assert(strstr(buf, "Fix:") != NULL);
   assert(strstr(buf, "max_delegation_spawns") != NULL);

   /* Unknown errors pass through unchanged, no Fix line. */
   delegation_augment_error("ECONNREFUSED upstream model", buf, sizeof(buf));
   assert(strcmp(buf, "ECONNREFUSED upstream model") == 0);
   assert(strstr(buf, "Fix:") == NULL);

   /* NULL message → empty out. */
   delegation_augment_error(NULL, buf, sizeof(buf));
   assert(buf[0] == '\0');

   /* Zero capacity is a no-op (must not crash). */
   delegation_augment_error("missing prompt", buf, 0);
}

/* Test that req_parent_depth (from cross-process request) is used correctly */
static void test_cross_process_depth_propagation(void)
{
   assert(db1_delegation_spawn_record("active-parent", NULL, "sess-depth", 1, "review") == 0);
   assert(db1_delegation_spawn_record("stale-parent", NULL, "sess-depth", 1, "review") == 0);
   assert(db1_delegation_spawn_complete("stale-parent") == 0);

   cJSON *jdepth = cJSON_CreateNumber(2);
   cJSON *jparent = cJSON_CreateString("active-parent");
   int req_parent_depth = 0;
   const char *request_parent = NULL;
   delegate_request_parent_context(jdepth, jparent, &req_parent_depth, &request_parent);
   assert(req_parent_depth == 2);
   assert(request_parent && strcmp(request_parent, "active-parent") == 0);
   cJSON_Delete(jparent);

   jparent = cJSON_CreateString("stale-parent");
   delegate_request_parent_context(jdepth, jparent, &req_parent_depth, &request_parent);
   assert(req_parent_depth == 0);
   assert(request_parent == NULL);

   tl_delegation_depth = 0;
   cJSON_Delete(jparent);
   cJSON_Delete(jdepth);
   printf("  PASS: test_cross_process_depth_propagation\n");
}

static void test_delegate_provider_route_override(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.default_agent, sizeof(cfg.default_agent), "mistral-slow");
   cfg.agent_count = 4;

   snprintf(cfg.agents[0].name, sizeof(cfg.agents[0].name), "openai-fast");
   snprintf(cfg.agents[0].provider, sizeof(cfg.agents[0].provider), "openai");
   snprintf(cfg.agents[0].roles[cfg.agents[0].role_count++], 32, "custom");
   cfg.agents[0].enabled = 1;
   cfg.agents[0].cost_tier = 0;

   snprintf(cfg.agents[1].name, sizeof(cfg.agents[1].name), "mistral-slow");
   snprintf(cfg.agents[1].provider, sizeof(cfg.agents[1].provider), "mistral");
   snprintf(cfg.agents[1].roles[cfg.agents[1].role_count++], 32, "custom");
   cfg.agents[1].enabled = 1;
   cfg.agents[1].cost_tier = 2;

   snprintf(cfg.agents[2].name, sizeof(cfg.agents[2].name), "mistral-cheap");
   snprintf(cfg.agents[2].provider, sizeof(cfg.agents[2].provider), "mistral");
   snprintf(cfg.agents[2].roles[cfg.agents[2].role_count++], 32, "custom");
   cfg.agents[2].enabled = 1;
   cfg.agents[2].cost_tier = 1;

   snprintf(cfg.agents[3].name, sizeof(cfg.agents[3].name), "mistral-other");
   snprintf(cfg.agents[3].provider, sizeof(cfg.agents[3].provider), "mistral");
   snprintf(cfg.agents[3].roles[cfg.agents[3].role_count++], 32, "other");
   cfg.agents[3].enabled = 1;
   cfg.agents[3].cost_tier = 0;

   char err[256];
   assert(delegate_apply_route_overrides(&cfg, "custom", NULL, -1, "mistral", NULL, err,
                                         sizeof(err)) == 0);
   assert(cfg.agents[0].enabled == 0);
   assert(cfg.agents[1].enabled == 0);
   assert(cfg.agents[2].enabled == 1);
   assert(cfg.agents[3].enabled == 0);

   memset(&cfg, 0, sizeof(cfg));
   assert(delegate_apply_route_overrides(&cfg, "custom", "one", -1, "mistral", NULL, err,
                                         sizeof(err)) == -1);
   assert(strstr(err, "--provider and --via") != NULL);
   printf("  PASS: test_delegate_provider_route_override\n");
}

static void test_delegate_generated_ids_are_unique(void)
{
   char prev[64] = "";
   for (int i = 0; i < 64; i++)
   {
      char id[64] = "";
      delegate_generate_id(id, sizeof(id));
      assert(strncmp(id, "deleg-", 6) == 0);
      assert(strlen(id) < sizeof(id));
      if (prev[0])
         assert(strcmp(prev, id) != 0);
      snprintf(prev, sizeof(prev), "%s", id);
   }
   printf("  PASS: test_delegate_generated_ids_are_unique\n");
}

static void test_delegate_status_handler(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   cJSON *req = cJSON_CreateObject();
   assert(handle_delegate_status(ctx, conn, req) == 0);
   assert(strcmp(g_last_error, "missing or invalid job_id") == 0);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", 777);
   assert(handle_delegate_status(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "not_found") ==
          0);
   cJSON_Delete(req);
   reset_last_response();

   int job_id = db1_agent_job_create("code", "run a test", "codex", "unit-test");
   assert(job_id > 0);
   db1_agent_job_update(job_id, "done", 3, "ok");

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", job_id);
   assert(handle_delegate_status(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   assert(cJSON_GetObjectItem(g_last_response, "job_id")->valueint == job_id);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "done") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "role")->valuestring, "code") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "agent_name")->valuestring, "codex") == 0);
   assert(cJSON_GetObjectItem(g_last_response, "cursor_turn")->valueint == 3);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "result")->valuestring, "ok") == 0);
   cJSON_Delete(req);
   reset_last_response();

   int raw_job_id =
       db1_agent_job_create("explain", "raw status regression", "minimax", "unit-test");
   assert(raw_job_id > 0);
   db1_agent_job_update(raw_job_id, "done", 0,
                        "I'll inspect it.\n[TOOL_CALL]\n{\"tool\":\"ReadFile\"}\n[/TOOL_CALL]");

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", raw_job_id);
   assert(handle_delegate_status(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "failed") == 0);
   assert(strstr(cJSON_GetObjectItem(g_last_response, "result")->valuestring,
                 "degenerate response") != NULL);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON *job_ids = cJSON_AddArrayToObject(req, "job_ids");
   cJSON_AddItemToArray(job_ids, cJSON_CreateNumber(job_id));
   cJSON_AddItemToArray(job_ids, cJSON_CreateNumber(777));
   assert(handle_delegate_status(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   cJSON *jobs = cJSON_GetObjectItem(g_last_response, "jobs");
   assert(cJSON_IsArray(jobs));
   assert(cJSON_GetArraySize(jobs) == 2);
   cJSON *first = cJSON_GetArrayItem(jobs, 0);
   cJSON *second = cJSON_GetArrayItem(jobs, 1);
   assert(cJSON_GetObjectItem(first, "job_id")->valueint == job_id);
   assert(strcmp(cJSON_GetObjectItem(first, "job_status")->valuestring, "done") == 0);
   assert(cJSON_GetObjectItem(second, "job_id")->valueint == 777);
   assert(strcmp(cJSON_GetObjectItem(second, "job_status")->valuestring, "not_found") == 0);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);

   printf("  PASS: test_delegate_status_handler\n");
}

static void test_delegate_background_handler(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "reviewer");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the background delegate test prompt");
   cJSON_AddTrueToObject(req, "background");

   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   cJSON *jid = cJSON_GetObjectItem(g_last_response, "job_id");
   assert(cJSON_IsNumber(jid));
   int job_id = jid->valueint;
   assert(job_id > 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "pending") == 0);

   db1_agent_job_t job;
   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "pending") == 0);
   assert(strcmp(job.role, "review") == 0);
   assert(strcmp(job.prompt, "run the background delegate test prompt") == 0);

   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   compute_ctx_t *submitted = (compute_ctx_t *)g_submitted_arg;
   assert(submitted->background_job_id == job_id);
   assert(submitted->conn_fd == -1);
   compute_ctx_free(submitted);
   g_submitted_arg = NULL;

   pthread_mutex_t mu;
   pthread_mutex_init(&mu, NULL);
   compute_ctx_t done_ctx = {.conn_fd = -1, .background_job_id = job_id, .write_mutex = &mu};
   cJSON *done = cJSON_CreateObject();
   cJSON_AddStringToObject(done, "status", "ok");
   cJSON_AddStringToObject(done, "response", "finished");
   cJSON_AddNumberToObject(done, "turns", 4);
   compute_respond(&done_ctx, done);
   pthread_mutex_destroy(&mu);

   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "done") == 0);
   assert(job.cursor_turn == 4);
   assert(strcmp(job.result, "finished") == 0);

   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_background_handler\n");
}

static void test_delegate_request_priority_defaults(void)
{
   compute_ctx_t interactive = {0};
   compute_ctx_t background = {.background_job_id = 42};

   assert(delegate_request_priority(&interactive, NULL) == CONCURRENCY_PRIORITY_INTERACTIVE);
   assert(delegate_request_priority(&background, NULL) == CONCURRENCY_PRIORITY_BACKGROUND);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "priority", -5);
   assert(delegate_request_priority(&background, cJSON_GetObjectItem(req, "priority")) == -5);
   cJSON_ReplaceItemInObject(req, "priority", cJSON_CreateNumber(5000));
   assert(delegate_request_priority(&interactive, cJSON_GetObjectItem(req, "priority")) == 1000);
   cJSON_ReplaceItemInObject(req, "priority", cJSON_CreateNumber(-5000));
   assert(delegate_request_priority(&interactive, cJSON_GetObjectItem(req, "priority")) == -1000);
   cJSON_Delete(req);

   printf("  PASS: test_delegate_request_priority_defaults\n");
}

static void test_chat_stream_dispatch_uses_dedicated_lane(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   ctx->compute_budget_total = 2;
   g_chat_dispatch_override = test_chat_dispatch_stub;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "message", "hello from the chat dispatch test");

   assert(handle_chat_send_stream(ctx, conn, req) == 0);
   assert(g_submitted_fn == chat_stream_worker);
   assert(g_submitted_arg != NULL);

   compute_ctx_t *submitted = (compute_ctx_t *)g_submitted_arg;
   assert(submitted->req != req);
   assert(cJSON_IsString(cJSON_GetObjectItem(submitted->req, "message")));
   compute_ctx_free(submitted);
   g_submitted_arg = NULL;

   cJSON_Delete(req);
   close(fds[0]);
   close(fds[1]);
   reset_last_response();
   free(conn);
   free(ctx);

   printf("  PASS: test_chat_stream_dispatch_uses_dedicated_lane\n");
}

static void test_chat_stream_trims_appended_transcript_jsonl(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   g_chat_dispatch_override = test_chat_dispatch_stub;

   const char *expected = "We need to improve the UX of the TUI quite significantly. "
                          "Implement this.";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "message",
                           "We need to improve the UX of the TUI quite significantly. "
                           "Implement this.\n"
                           "{\"text\":\"Create a PR and merge.\",\"ts\":1779277477}\n"
                           "{\"text\":\"What were your instructions?\",\"ts\":1779278062}");

   assert(handle_chat_send_stream(ctx, conn, req) == 0);
   compute_ctx_t *submitted = (compute_ctx_t *)g_submitted_arg;
   cJSON *msg = cJSON_GetObjectItemCaseSensitive(submitted->req, "message");
   assert(cJSON_IsString(msg));
   assert(strcmp(msg->valuestring, expected) == 0);

   compute_ctx_free(submitted);
   g_submitted_arg = NULL;
   cJSON_Delete(req);
   close(fds[0]);
   close(fds[1]);
   free(conn);
   free(ctx);

   printf("  PASS: test_chat_stream_trims_appended_transcript_jsonl\n");
}

static void test_chat_stream_trims_malformed_appended_transcript_head(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   g_chat_dispatch_override = test_chat_dispatch_stub;

   const char *expected = "We need to improve the UX of the TUI quite significantly. "
                          "Implement this.";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "message",
                           "We need to improve the UX of the TUI quite significantly. "
                           "Implement this.@\":\"Create a PR and merge.\",\"ts\":1779277477}\n"
                           "{\"text\":\"What were your instructions?\",\"ts\":1779278062}");

   assert(handle_chat_send_stream(ctx, conn, req) == 0);
   compute_ctx_t *submitted = (compute_ctx_t *)g_submitted_arg;
   cJSON *msg = cJSON_GetObjectItemCaseSensitive(submitted->req, "message");
   assert(cJSON_IsString(msg));
   assert(strcmp(msg->valuestring, expected) == 0);

   compute_ctx_free(submitted);
   g_submitted_arg = NULL;
   cJSON_Delete(req);
   close(fds[0]);
   close(fds[1]);
   free(conn);
   free(ctx);

   printf("  PASS: test_chat_stream_trims_malformed_appended_transcript_head\n");
}

static void test_chat_thread_reserve_has_no_admission_cap(void)
{
   server_ctx_t ctx = {0};
   ctx.compute_budget_total = 1;

   pthread_mutex_lock(&g_chat_threads_lock);
   g_chat_threads_active = 1;
   pthread_mutex_unlock(&g_chat_threads_lock);

   assert(chat_thread_reserve(&ctx) == 0);

   pthread_mutex_lock(&g_chat_threads_lock);
   assert(g_chat_threads_active == 2);
   pthread_mutex_unlock(&g_chat_threads_lock);

   chat_thread_release();
   chat_thread_release();

   pthread_mutex_lock(&g_chat_threads_lock);
   assert(g_chat_threads_active == 0);
   pthread_cond_broadcast(&g_chat_threads_idle);
   pthread_mutex_unlock(&g_chat_threads_lock);

   reset_last_response();

   printf("  PASS: test_chat_thread_reserve_has_no_admission_cap\n");
}

static void test_tool_execute_dispatch_uses_dedicated_lane(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   ctx->compute_budget_total = 2;
   g_tool_dispatch_override = test_tool_dispatch_stub;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "tool", "bash");
   cJSON_AddStringToObject(req, "arguments", "{}");
   cJSON_AddStringToObject(req, "session_id", "tool-dispatch-test");

   assert(handle_tool_execute(ctx, conn, req) == 0);
   assert(g_submitted_fn == tool_execute_worker);
   assert(g_submitted_arg != NULL);

   compute_ctx_t *submitted = (compute_ctx_t *)g_submitted_arg;
   assert(submitted->req != req);
   assert(cJSON_IsString(cJSON_GetObjectItem(submitted->req, "tool")));
   compute_ctx_free(submitted);
   g_submitted_arg = NULL;

   cJSON_Delete(req);
   close(fds[0]);
   close(fds[1]);
   reset_last_response();
   free(conn);
   free(ctx);

   printf("  PASS: test_tool_execute_dispatch_uses_dedicated_lane\n");
}

static void test_tool_execute_reserve_is_not_process_global_cap(void)
{
   server_ctx_t ctx = {0};
   ctx.session_threads = 4;

   pthread_mutex_lock(&g_tool_threads_lock);
   g_tool_threads_active = 99;
   pthread_mutex_unlock(&g_tool_threads_lock);

   assert(tool_thread_limit(&ctx) == 4);
   assert(tool_thread_reserve(&ctx) == 0);

   pthread_mutex_lock(&g_tool_threads_lock);
   assert(g_tool_threads_active == 100);
   g_tool_threads_active = 0;
   pthread_cond_broadcast(&g_tool_threads_idle);
   pthread_mutex_unlock(&g_tool_threads_lock);

   printf("  PASS: test_tool_execute_reserve_is_not_process_global_cap\n");
}

static void test_async_lane_limits_report_session_pool_defaults(void)
{
   server_ctx_t ctx = {0};

   ctx.compute_budget_total = 32;
   assert(chat_thread_limit(&ctx) == CONFIG_DEFAULT_SESSION_THREADS);
   assert(tool_thread_limit(&ctx) == CONFIG_DEFAULT_SESSION_THREADS);

   ctx.session_threads = 6;
   assert(chat_thread_limit(&ctx) == 6);
   assert(tool_thread_limit(&ctx) == 6);

   printf("  PASS: test_async_lane_limits_report_session_pool_defaults\n");
}

static void test_delegate_releases_compute_budget_before_provider_wait(void)
{
   reset_last_response();
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   g_agent_response = "delegate completed";

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "run a delegate that verifies compute budget release timing");

   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;

   assert(g_budget_acquire_calls >= 1);
   assert(g_budget_release_calls >= 1);
   assert(g_agent_run_seen_compute_override == 0);
   assert(strstr(g_last_system_prompt, "# Code Principles\n") == g_last_system_prompt);
   assert(strstr(g_last_system_prompt, "Prefer composition over inheritance") != NULL);

   close(fds[0]);
   close(fds[1]);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);

   printf("  PASS: test_delegate_releases_compute_budget_before_provider_wait\n");
}

static void test_background_error_preserves_cursor(void)
{
   int job_id =
       db1_agent_job_create("code", "run a cursor preservation test", "codex", "unit-test");
   assert(job_id > 0);
   db1_agent_job_update(job_id, "running", 9, "prior progress");

   pthread_mutex_t mu;
   pthread_mutex_init(&mu, NULL);
   compute_ctx_t err_ctx = {.conn_fd = -1, .background_job_id = job_id, .write_mutex = &mu};
   cJSON *err = cJSON_CreateObject();
   cJSON_AddStringToObject(err, "status", "error");
   cJSON_AddStringToObject(err, "message", "failed after progress");
   compute_respond(&err_ctx, err);

   db1_agent_job_t job;
   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "failed") == 0);
   assert(job.cursor_turn == 9);
   assert(strcmp(job.result, "failed after progress") == 0);

   err = cJSON_Parse("{\"status\":\"error\",\"message\":\"err\",\"response\":\"ok\",\"turns\":7}");
   assert(err != NULL);
   compute_respond(&err_ctx, err);
   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "partial") == 0 && job.cursor_turn == 7);
   assert(strstr(job.result, "err") && strstr(job.result, "ok"));
   pthread_mutex_destroy(&mu);

   printf("  PASS: test_background_error_preserves_cursor\n");
}

static cJSON *add_launch_packet(cJSON *packets, const char *id, const char *title, const char *file)
{
   cJSON *packet = cJSON_CreateObject();
   cJSON_AddStringToObject(packet, "id", id);
   cJSON_AddStringToObject(packet, "role", "code");
   cJSON_AddStringToObject(packet, "title", title);
   cJSON_AddStringToObject(packet, "objective", "complete the packet");
   cJSON *owned = cJSON_AddArrayToObject(packet, "owned_files");
   cJSON_AddItemToArray(owned, cJSON_CreateString(file));
   cJSON_AddArrayToObject(packet, "acceptance_criteria");
   cJSON_AddArrayToObject(packet, "verify_commands");
   cJSON_AddStringToObject(packet, "handoff_schema", "delegate_result_v1");
   cJSON_AddItemToArray(packets, packet);
   return packet;
}

static void test_delegate_launch_creates_coord_job(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "parallel", 2);
   cJSON *plan = cJSON_AddObjectToObject(req, "plan");
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON_AddStringToObject(plan, "title", "Launch Test Plan");
   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");
   add_launch_packet(packets, "packet-one", "Edit foo", "src/foo.c");
   add_launch_packet(packets, "packet-two", "Edit bar", "src/bar.c");

   cJSON *review = cJSON_CreateObject();
   cJSON_AddStringToObject(review, "id", "packet-reviewer");
   cJSON_AddStringToObject(review, "role", "review");
   cJSON_AddArrayToObject(review, "owned_files");
   cJSON_AddItemToArray(packets, review);

   assert(handle_delegate_launch(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   assert(cJSON_GetObjectItem(g_last_response, "tasks")->valueint == 2);
   assert(cJSON_GetObjectItem(g_last_response, "max_concurrent")->valueint == 2);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "pending") == 0);
   assert(strstr(cJSON_GetObjectItem(g_last_response, "status_command")->valuestring,
                 "aimee job status") != NULL);
   int plan_id = cJSON_GetObjectItem(g_last_response, "plan_id")->valueint;
   int job_id = cJSON_GetObjectItem(g_last_response, "job_id")->valueint;
   assert(plan_id > 0 && job_id > 0);

   plan_t stored_plan;
   assert(db1_execution_plan_get(plan_id, &stored_plan) == 0);
   assert(stored_plan.step_count == 2);

   db1_coord_job_t job;
   assert(db1_coord_job_get(job_id, &job) == 0);
   assert(job.plan_id == plan_id);
   assert(job.max_concurrent == 2);
   assert(job.total_tasks == 2);

   db1_coord_task_t tasks[4];
   int count = db1_coord_job_list_tasks(job_id, tasks, 4);
   assert(count == 2);
   assert(strstr(tasks[0].files, "src/foo.c") != NULL);
   assert(strstr(tasks[1].files, "src/bar.c") != NULL);
   assert(tasks[0].step_id == stored_plan.steps[0].id);
   assert(tasks[1].step_id == stored_plan.steps[1].id);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_launch_creates_coord_job\n");
}

static void test_delegate_launch_rejects_packet_without_owned_files(void)
{
   reset_last_response();
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *req = cJSON_CreateObject();
   cJSON *plan = cJSON_AddObjectToObject(req, "plan");
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON_AddStringToObject(plan, "title", "Invalid Launch Plan");
   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");
   cJSON *packet = cJSON_CreateObject();
   cJSON_AddStringToObject(packet, "id", "packet-bad");
   cJSON_AddStringToObject(packet, "role", "code");
   cJSON_AddStringToObject(packet, "title", "Missing owned files");
   cJSON_AddItemToArray(packets, packet);
   assert(handle_delegate_launch(ctx, conn, req) == 0);
   assert(g_last_response == NULL);
   assert(strcmp(g_last_error, "delegate plan packet missing owned_files") == 0);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_launch_rejects_packet_without_owned_files\n");
}
static void test_delegate_launch_rejects_packet_without_handoff_schema(void)
{
   reset_last_response();
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *req = cJSON_CreateObject();
   cJSON *plan = cJSON_AddObjectToObject(req, "plan");
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON_AddStringToObject(plan, "title", "Invalid Launch Plan");
   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");
   cJSON *packet = cJSON_CreateObject();
   cJSON_AddStringToObject(packet, "id", "packet-bad-handoff");
   cJSON_AddStringToObject(packet, "role", "code");
   cJSON_AddStringToObject(packet, "title", "Missing handoff schema");
   cJSON *owned = cJSON_AddArrayToObject(packet, "owned_files");
   cJSON_AddItemToArray(owned, cJSON_CreateString("src/foo.c"));
   cJSON_AddItemToArray(packets, packet);
   assert(handle_delegate_launch(ctx, conn, req) == 0);
   assert(g_last_response == NULL);
   assert(strcmp(g_last_error, "delegate plan packet missing handoff_schema delegate_result_v1") ==
          0);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_launch_rejects_packet_without_handoff_schema\n");
}

/* Async-only (WP-B): handle_delegate returns a {job_id,"pending"} envelope,
 * captured by the stubbed server_send_ok into g_last_response (NOT written to the
 * connection — the worker's compute_respond persists status/result to the job row
 * instead, so the test pipe stays empty). Load the job named by that envelope's
 * job_id. An error delegate's message lands in job.result (so strstr(job.result,
 * msg) is a status-agnostic check); a successful delegate is status "done". */
static int delegate_current_job(db1_agent_job_t *out_job)
{
   assert(g_last_response != NULL);
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(g_last_response, "job_id");
   assert(cJSON_IsNumber(jid));
   return db1_agent_job_get(jid->valueint, out_job);
}

#include "test_server_compute_handoff.inc"
#include "test_server_compute_delegate_write.inc"
#include "test_server_compute_liveness.inc"
#include "test_server_compute_state_invariants.inc"

/* Pure prompt helpers extracted from delegate_worker (server/delegate_prompt.c). */
char *delegate_rewrite_prompt_cwd(const char *prompt, const char *cwd, const char *worktree_path,
                                  int *occurrences_out);
char *delegate_prompt_append_block(const char *base, const char *block);

static void test_delegate_rewrite_prompt_cwd(void)
{
   /* No occurrence -> NULL, count 0, original kept by caller. */
   int n = 7;
   char *r = delegate_rewrite_prompt_cwd("nothing here", "/old", "/new", &n);
   assert(r == NULL && n == 0);

   /* Single occurrence. */
   r = delegate_rewrite_prompt_cwd("see /old/a.c please", "/old", "/new/wt", &n);
   assert(r != NULL && n == 1);
   assert(strcmp(r, "see /new/wt/a.c please") == 0);
   free(r);

   /* Multiple occurrences, all rewritten; length math holds for grow + shrink. */
   r = delegate_rewrite_prompt_cwd("/old x /old y /old", "/old", "/longer", &n);
   assert(r != NULL && n == 3);
   assert(strcmp(r, "/longer x /longer y /longer") == 0);
   free(r);
   r = delegate_rewrite_prompt_cwd("/oldpath/a /oldpath/b", "/oldpath", "/p", &n);
   assert(r != NULL && n == 2);
   assert(strcmp(r, "/p/a /p/b") == 0);
   free(r);

   /* Defensive: empty/NULL inputs -> NULL, count 0. */
   assert(delegate_rewrite_prompt_cwd(NULL, "/old", "/new", &n) == NULL && n == 0);
   assert(delegate_rewrite_prompt_cwd("x", "", "/new", &n) == NULL && n == 0);
   assert(delegate_rewrite_prompt_cwd("x", "/old", "", &n) == NULL && n == 0);
   /* NULL occurrences_out is allowed. */
   r = delegate_rewrite_prompt_cwd("/old", "/old", "/new", NULL);
   assert(r != NULL && strcmp(r, "/new") == 0);
   free(r);
   printf("  PASS: test_delegate_rewrite_prompt_cwd\n");
}

static void test_delegate_prompt_append_block(void)
{
   /* NULL base treated as empty. */
   char *r = delegate_prompt_append_block(NULL, "tail");
   assert(r != NULL && strcmp(r, "tail") == 0);
   free(r);

   /* Order preserved: base then block. */
   r = delegate_prompt_append_block("head", "\n\ntail");
   assert(r != NULL && strcmp(r, "head\n\ntail") == 0);
   free(r);

   /* Empty block yields a copy of base; NULL block -> NULL. */
   r = delegate_prompt_append_block("base", "");
   assert(r != NULL && strcmp(r, "base") == 0);
   free(r);
   assert(delegate_prompt_append_block("base", NULL) == NULL);
   printf("  PASS: test_delegate_prompt_append_block\n");
}

/* WP-C.0 hop 3 of 3: create_compute_ctx must copy the attested vault identity
 * from the (still-live) conn into the compute_ctx, so the detached worker — which
 * runs after conn_fd is closed and no thread-local survives — can resolve the
 * right per-user vault from the only identity key it is allowed to trust. */
static void test_create_compute_ctx_threads_vault_identity(void)
{
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   conn.fd = -1; /* dup(-1) -> -1, fine for this no-I/O construction test */
   conn.attested_transport = ATTEST_UDS_PEERCRED;
   snprintf(conn.vault_principal, sizeof(conn.vault_principal), "uid:1234");

   cJSON *req = cJSON_CreateObject();
   compute_ctx_t *cctx = create_compute_ctx(NULL, &conn, req);
   assert(cctx != NULL);
   assert(cctx->attested_transport == ATTEST_UDS_PEERCRED);
   assert(strcmp(cctx->vault_principal, "uid:1234") == 0);
   compute_ctx_free(cctx);
   cJSON_Delete(req);

   /* A conn with no attested identity (the un-attested default) yields an empty
    * principal => no vault (fail-closed). */
   server_conn_t bare;
   memset(&bare, 0, sizeof(bare));
   bare.fd = -1;
   cJSON *req2 = cJSON_CreateObject();
   compute_ctx_t *c2 = create_compute_ctx(NULL, &bare, req2);
   assert(c2 != NULL);
   assert(c2->attested_transport == ATTEST_NONE);
   assert(c2->vault_principal[0] == '\0');
   compute_ctx_free(c2);
   cJSON_Delete(req2);
   printf("  PASS: test_create_compute_ctx_threads_vault_identity\n");
}

int main(void)
{
   /* DB1 owns delegation_spawns + delegation_messages. */
   assert(db1_init(":memory:") == 0);
   test_delegate_rewrite_prompt_cwd();
   test_delegate_prompt_append_block();
   test_mailbox_lifecycle();
   test_reply_wakeup();
   test_timeout_and_no_mailbox();
   test_message_recording();
   test_write_all_handles_eagain();
   test_compute_ctx_release_budget_clears_grant_and_override();
   test_depth_limit_enforcement();
   test_spawn_count_total_tracking();
   test_spawn_record_marks_running();
   test_spawn_limit_descendants_blocks_completed_chain();
   test_depth_context_save_restore();
   test_delegation_augment_error();
   test_cross_process_depth_propagation();
   test_delegate_provider_route_override();
   test_delegate_generated_ids_are_unique();
   test_delegate_status_handler();
   test_delegate_background_handler();
   test_delegate_request_priority_defaults();
   test_chat_stream_dispatch_uses_dedicated_lane();
   test_chat_stream_trims_appended_transcript_jsonl();
   test_chat_stream_trims_malformed_appended_transcript_head();
   test_chat_thread_reserve_has_no_admission_cap();
   test_tool_execute_dispatch_uses_dedicated_lane();
   test_tool_execute_reserve_is_not_process_global_cap();
   test_async_lane_limits_report_session_pool_defaults();
   test_delegate_releases_compute_budget_before_provider_wait();
   test_background_error_preserves_cursor();
   test_background_ok_rejects_raw_tool_call_markup();
   test_background_ok_rejects_unexecuted_tool_plan();
   test_direct_delegate_rejects_raw_tool_call_markup();
   test_delegate_launch_creates_coord_job();
   test_delegate_launch_repairs_paths_from_request_cwd();
   test_delegate_launch_rejects_packet_without_owned_files();
   test_delegate_launch_rejects_packet_without_handoff_schema();
   test_direct_delegate_handoff_json_response();
   test_direct_delegate_handoff_repair_attempt();
   test_direct_delegate_handoff_repair_failure_is_error();
   test_direct_delegate_review_auto_tools_uses_tools();
   test_direct_delegate_reviewer_alias_auto_tools_uses_tools();
   test_direct_delegate_explicit_tools_forces_tools();
   test_direct_delegate_one_turn_diagnose_suppresses_default_tools();
   test_readonly_code_delegate_disables_write_enforce();
   test_readonly_refactor_delegate_disables_write_enforce();
   test_direct_delegate_max_turns_override();
   test_read_only_delegate_uses_parent_workspace();
   test_read_only_branch_delegate_rejected();
   test_inspection_roles_get_evidence_bundle();
   test_delegate_worker_restores_caller_context();
   test_delegate_worker_balances_concurrency_slot();
   test_delegate_worker_ok_response_shape();
   test_delegate_worker_restores_state_on_error();
   test_delegate_worker_sets_session_override_during_run();
   test_delegate_worker_concurrency_cancelled_restores();
   test_delegate_worker_concurrency_queue_full_errors();
   test_create_compute_ctx_threads_vault_identity();
   db1_shutdown();
   reset_last_response();
   printf("server_compute: all tests passed\n");
   return 0;
}
