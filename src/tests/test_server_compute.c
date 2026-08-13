#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "aimee.h"
#include <aimee/delegates/module_api.h>
#include "delegate_permissions_stub.h"

/* When set, the launch-plan stub near the bottom of this file refuses with this
 * wording and returns no rows. It is how the refusal test provokes a refusal
 * without deciding here what deserves one. */
static const char *g_launch_refusal;
#include "db.h"
#include "db_schema.h"
#include "db1.h"
#include "server.h"
#include <aimee/audit/obs_bus.h>
#include "platform_path.h"
#include "platform_test_util.h"
#include <aimee/delegates/delegate_backend.h>
#include <aimee/delegates/delegate_launch_args.h>

/* A delegate runs in its own container or not at all, so a delegation with no
 * container runtime now REFUSES rather than falling back to running in-process
 * on the host. These cases are about compute-budget and dispatch behaviour, not
 * about sandboxing, so they need a box that HAS a runtime. Register a fake one
 * under the name the seam looks up. */
static int g_fake_container_state = 1;

static int fake_docker_acquire(delegate_backend_t *self, const char *task_id,
                               const delegate_backend_config_t *cfg, void **out)
{
   (void)self;
   (void)task_id;
   (void)cfg;
   *out = &g_fake_container_state;
   return 0;
}

static void fake_docker_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   (void)state;
   (void)hibernate;
}

static int fake_docker_exec(delegate_backend_t *self, void *state, const char *command,
                            int timeout_ms, delegate_exec_result_t *r)
{
   (void)self;
   (void)state;
   (void)command;
   (void)timeout_ms;
   if (r)
      r->exit_code = 0;
   return 0;
}

static delegate_backend_t g_fake_docker = {.name = "docker",
                                           .description = "fake docker for tests",
                                           .acquire = fake_docker_acquire,
                                           .release = fake_docker_release,
                                           .exec = fake_docker_exec,
                                           .read_file = NULL,
                                           .write_file = NULL,
                                           .list_dir = NULL,
                                           .get_cwd = NULL,
                                           .set_cwd = NULL};

/* The harness sets TMPDIR, and a tree outside the REGISTERED workspace roots is
 * now refused outright rather than quietly run in-process. So these fixtures live
 * under the registered temp root instead of a hard-coded /tmp. */
static const char *test_parent_repo(void)
{
   static char buf[600];
   if (!buf[0])
      snprintf(buf, sizeof(buf), "%s/aimee-parent-repo", platform_tmpdir());
   return buf;
}

static const char *test_delegate_wt(void)
{
   static char buf[600];
   if (!buf[0])
      snprintf(buf, sizeof(buf), "%s/aimee-delegate-wt", platform_tmpdir());
   return buf;
}

/* Registered workspace roots are now a PREREQUISITE for delegation, not just for
 * sandboxing: an unregistered tree is refused outright rather than quietly run
 * in-process. These cases use /tmp paths as their workspace, so register /tmp. */
static void register_test_workspace_root(void)
{
   static char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-compute-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   char cfgdir[640];
   snprintf(cfgdir, sizeof(cfgdir), "mkdir -p %s/.config/aimee", tmpdir);
   assert(system(cfgdir) == 0);

   char cfgpath[700];
   snprintf(cfgpath, sizeof(cfgpath), "%s/.config/aimee/aimee.yaml", tmpdir);
   FILE *cf = fopen(cfgpath, "w");
   assert(cf != NULL);
   fprintf(cf, "workspaces:\n  - path: %s\n", platform_tmpdir());
   fclose(cf);
   config_reload();
   assert(config_workspace_count() == 1);
}

/* The learned toolchain moved to the sandbox module, so resolving a sandbox
 * image now asks the bus for it. No bus runs in a unit test: report the module
 * as unattached, which is the condition these cases already assume (an image
 * resolved with no learned packages). */
int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return 0;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)event_kind;
   (void)stage_id;
   (void)trace_id;
   (void)deadline_ns;
   (void)request_body;
   (void)request_len;
   (void)response_body;
   (void)response_capacity;
   (void)response_len;
   (void)cancelled;
   (void)cancel_context;
   return AIMEE_MODULE_CALL_TRANSPORT;
}

#include "vault_service.h" /* vault_status_t for the inject-api-key stub */
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
/* server_compute.c's former .inc fragments are now sibling TUs; the white-box
 * test pulls them into this TU the same way it used to get them via the .inc. */
#include "../server/server_compute_mailbox.c"
#include "../server_compute_episodes.c"
#include "../server/server_compute_roundtable.c"
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
/* Tracks agent_run_force_no_tools: g_force_no_tools is the live thread-local
 * stand-in; g_agent_run_seen_no_tools latches its value at the agent_run call so
 * a test can assert the delegate no-tools path forced tools off for the turn. */
static int g_force_no_tools = 0;
static int g_agent_run_seen_no_tools = 0;
static int g_last_agent_max_turns = -999;
static int g_last_request_tool_loop_cap = -999;
static int64_t g_last_request_tool_loop_deadline = -999;
static int g_last_agent_tool_loop_cap = -999;
static int g_last_write_enforce = -999;
static int g_budget_acquire_calls = 0;
static int g_budget_release_calls = 0;
static int g_budget_last_grant = 0;
static int g_agent_run_seen_compute_override = -999;
static int g_agent_seen_compute_override = -999;
static int g_agent_seen_budget_release_calls = -999;
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
/* The two roots of one delegate turn, sampled while the turn is running: the
 * directory the shell would execute in, and the directory the file tools are
 * permitted to write to. They must name the same tree. */
/* The "## Working root" block as the delegate received it. Captured from the
 * FULL system prompt rather than read back out of g_last_system_prompt, which is
 * a fixed 4KB and would truncate away a block appended at the end. */
static char g_last_root_notice[1024];
static char g_shell_root_during_run[MAX_PATH_LEN];
static char g_file_write_root[MAX_PATH_LEN];
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
   /* Sample the shell's root HERE rather than after the turn: delegate_worker
    * clears it on the way out, so afterwards there is nothing left to compare. */
   {
      const char *rc = run_cmd_get_cwd();
      snprintf(g_shell_root_during_run, sizeof(g_shell_root_during_run), "%s", rc ? rc : "");
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

/* P1: controllable fixtures for the codex-oauth vault fallback test (default values
 * reproduce the prior always-miss stub behaviour, so existing tests are unaffected). */
static int g_codex_set_called;
static char g_codex_set_token[256];
static char g_codex_set_account[256];
static vault_status_t g_codex_perturn_status = VAULT_NO_ENTRY;
static vault_status_t g_codex_srv_status = VAULT_NO_ENTRY;
static const char *g_codex_perturn_token = "";
static const char *g_codex_srv_token = "";
static void codex_fixture_reset(void)
{
   g_codex_set_called = 0;
   g_codex_set_token[0] = '\0';
   g_codex_set_account[0] = '\0';
   g_codex_perturn_status = VAULT_NO_ENTRY;
   g_codex_srv_status = VAULT_NO_ENTRY;
   g_codex_perturn_token = "";
   g_codex_srv_token = "";
}
void agent_set_request_codex_creds(const char *token, const char *account_id)
{
   g_codex_set_called++;
   if (token)
      snprintf(g_codex_set_token, sizeof(g_codex_set_token), "%s", token);
   if (account_id)
      snprintf(g_codex_set_account, sizeof(g_codex_set_account), "%s", account_id);
}
int agent_request_codex_token_present(void)
{
   return 0;
}
void agent_set_request_session(const char *session_id)
{
   (void)session_id;
}
/* Functional stub: store the per-turn vault principal so the create_compute_ctx
 * fallback (WP-C.2c(3)) is exercisable. */
static char g_stub_vault_principal[VAULT_PRINCIPAL_MAX];
void agent_set_request_vault_principal(const char *principal)
{
   snprintf(g_stub_vault_principal, sizeof(g_stub_vault_principal), "%s",
            principal ? principal : "");
}
const char *agent_get_request_vault_principal(void)
{
   return g_stub_vault_principal;
}

/* Delegate per-turn heartbeat (server_delegate_monitor.o not linked here). */
void server_delegate_heartbeat_begin(int job_id)
{
   (void)job_id;
}
void server_delegate_heartbeat_end(void)
{
}

static int g_agent_supports_code = 0;
/* The single role the stub agent advertises, when a test needs one other than
 * "code". Without this a delegate for any other role is rejected as unroutable
 * before it runs, so a test asserting on the RUN silently asserts on nothing. */
static char g_agent_role[32] = "";
int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->agent_count = 1;
   snprintf(cfg->agents[0].name, sizeof(cfg->agents[0].name), "test-agent");
   snprintf(cfg->agents[0].model, sizeof(cfg->agents[0].model), "gpt-4o");
   snprintf(cfg->agents[0].provider, sizeof(cfg->agents[0].provider), "openai");
   cfg->agents[0].enabled = 1;
   cfg->agents[0].tools_enabled = g_config_tools_enabled;
   cfg->agents[0].max_tokens = 4096;
   cfg->agents[0].max_turns = -1;
   if (g_agent_role[0])
   {
      snprintf(cfg->agents[0].roles[0], sizeof(cfg->agents[0].roles[0]), "%s", g_agent_role);
      cfg->agents[0].role_count = 1;
   }
   else if (g_agent_supports_code)
   {
      snprintf(cfg->agents[0].roles[0], sizeof(cfg->agents[0].roles[0]), "code");
      cfg->agents[0].role_count = 1;
   }
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
void delegate_apply_max_turns_cap(agent_config_t *cfg, const char *role, int cap)
{
   (void)role;
   if (!cfg || cap <= 0)
      return;
   for (int i = 0; i < cfg->agent_count; i++)
      if (cfg->agents[i].max_turns <= 0 || cfg->agents[i].max_turns > cap)
         cfg->agents[i].max_turns = cap;
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

/* The role definition an operator wrote, if these fixtures set one. NULL is the
 * usual case: no operator wrote a role, so every role is the one that ships.
 *
 * What a definition MEANS is proved where it is read (the parse, in
 * server-go/modules/delegates/roledefinition_test.go) and where it is loaded
 * (the handover, in unit-test-role-templates). What is proved HERE is that it
 * reaches the delegate and changes what the delegate is given. */
static const char *g_role_definition;

char *role_template_frontmatter(const char *project_root, const char *role)
{
   (void)project_root;
   (void)role;
   return g_role_definition ? strdup(g_role_definition) : NULL;
}

void agent_http_init(void)
{
}

/* WP-C.1: the delegate worker calls vault_service_inject_api_key, but these tests
 * always run with an empty cctx->vault_principal so the vault branch is skipped;
 * stub it to resolve the symbol (linking the real one would pull in the crypto +
 * config chain). */
vault_status_t vault_service_inject_api_key(const char *principal, const char *agent, char *api_key,
                                            size_t api_key_len, long now_epoch)
{
   (void)principal;
   (void)agent;
   (void)api_key;
   (void)api_key_len;
   (void)now_epoch;
   return VAULT_NO_ENTRY;
}
/* WP-C.3: delegate_credential_retry.o (linked here) calls vault_service_get for
 * the codex-oauth override; these tests run keyless, so stub it to a miss. */
vault_status_t vault_service_get(const char *principal, const char *agent, const char *cred,
                                 char *out, size_t out_cap, long now_epoch)
{
   (void)principal;
   (void)agent;
   (void)now_epoch;
   if (out && out_cap)
      out[0] = '\0';
   if (g_codex_perturn_status == VAULT_OK && cred && strcmp(cred, VAULT_CODEX_TOKEN_CRED) == 0 &&
       out && out_cap)
      snprintf(out, out_cap, "%s", g_codex_perturn_token);
   return g_codex_perturn_status;
}
/* cred-vault-consolidation P1: the codex-oauth override now also probes the server
 * principal; controllable via g_codex_srv_* (defaults to a miss). */
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   (void)agent;
   if (out && out_len)
      out[0] = '\0';
   if (g_codex_srv_status == VAULT_OK && cred && strcmp(cred, VAULT_CODEX_TOKEN_CRED) == 0 && out &&
       out_len)
      snprintf(out, out_len, "%s", g_codex_srv_token);
   return g_codex_srv_status;
}

int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt, const char *prompt,
              int max_tokens, agent_result_t *result)
{
   (void)role;
   (void)prompt;
   (void)max_tokens;
   if (system_prompt)
      snprintf(g_last_system_prompt, sizeof(g_last_system_prompt), "%s", system_prompt);
   {
      const char *wr = system_prompt ? strstr(system_prompt, "## Working root") : NULL;
      snprintf(g_last_root_notice, sizeof(g_last_root_notice), "%s", wr ? wr : "");
   }
   memset(result, 0, sizeof(*result));
   g_agent_run_calls++;
   g_agent_run_seen_no_tools = g_force_no_tools;
   g_agent_run_seen_compute_override = g_aimee_compute_threads_override;
   g_last_agent_max_turns = cfg->agent_count > 0 ? cfg->agents[0].max_turns : -999;
   g_last_request_tool_loop_cap = cfg->tool_loop_timeout_ms_cap;
   g_last_request_tool_loop_deadline = cfg->tool_loop_deadline_ms;
   g_last_agent_tool_loop_cap =
       cfg->agent_count > 0 ? cfg->agents[0].tool_loop_timeout_ms_cap : -999;
   g_agent_seen_compute_override = g_aimee_compute_threads_override;
   g_agent_seen_budget_release_calls = g_budget_release_calls;
   fake_agent_fill_response(prompt, result);
   if (g_agent_run_rc != 0)
      snprintf(result->error, sizeof(result->error), "stubbed agent failure");
   return g_agent_run_rc;
}

void agent_run_force_no_tools(int on)
{
   g_force_no_tools = on ? 1 : 0;
}

int agent_run_with_tools(agent_config_t *cfg, const char *role, const char *system_prompt,
                         const char *prompt, int max_tokens, agent_result_t *result)
{
   (void)role;
   (void)prompt;
   (void)max_tokens;
   if (system_prompt)
      snprintf(g_last_system_prompt, sizeof(g_last_system_prompt), "%s", system_prompt);
   {
      const char *wr = system_prompt ? strstr(system_prompt, "## Working root") : NULL;
      snprintf(g_last_root_notice, sizeof(g_last_root_notice), "%s", wr ? wr : "");
   }
   memset(result, 0, sizeof(*result));
   g_agent_tool_run_calls++;
   g_agent_run_seen_compute_override = g_aimee_compute_threads_override;
   g_last_agent_max_turns = cfg->agent_count > 0 ? cfg->agents[0].max_turns : -999;
   g_last_request_tool_loop_cap = cfg->tool_loop_timeout_ms_cap;
   g_last_request_tool_loop_deadline = cfg->tool_loop_deadline_ms;
   g_last_agent_tool_loop_cap =
       cfg->agent_count > 0 ? cfg->agents[0].tool_loop_timeout_ms_cap : -999;
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
   /* The root the file tools are allowed to write to for this turn. Recorded so a
    * test can compare it against the root the shell actually runs in --
    * see test_delegate_shell_and_file_roots_agree. */
   snprintf(g_file_write_root, sizeof(g_file_write_root), "%s", w ? w : (r ? r : ""));
}
void agent_tools_parent_write_guard_clear(void)
{
}
void agent_tools_write_capable_set(int capable)
{
   (void)capable;
}
/* Per-turn toolset override. Stubbed alongside the write guard because this test
 * links server_compute without the agent-tools TU; the real one is thread-local
 * (see agent_tools.c) precisely because delegate turns overlap on pooled threads. */
void agent_tools_set_active_toolset(const char *toolset)
{
   (void)toolset;
}
const char *agent_tools_active_toolset(void)
{
   return NULL;
}
int agent_tools_readonly_delegate_blocks(void)
{
   return 0;
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
/* server_compute resolves the two routing settings through this now instead of
 * loading a config_t. Both off, matching the zeroed config_t the compute path
 * used to build here. */
void agent_route_policy_current(agent_route_policy_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
}

agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   (void)role;
   return cfg && cfg->agent_count > 0 ? &cfg->agents[0] : NULL;
}
agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role,
                               const agent_route_policy_t *sys_cfg, unsigned required_caps,
                               int min_context)
{
   (void)sys_cfg;
   (void)required_caps;
   (void)min_context;
   return agent_route(cfg, role);
}
/* The compute path now routes through the SCOPED variant so a packet's scope
 * ceiling is enforced server-side. Honour the ceiling in the stub rather than
 * ignoring the argument: routing that silently admitted an over-scope seat is
 * exactly the bug this replaced, and a stub that drops the parameter could not
 * catch a regression to it. */
agent_t *agent_route_with_caps_scoped(agent_config_t *cfg, const char *role,
                                      const agent_route_policy_t *sys_cfg, unsigned required_caps,
                                      int min_context, agent_scope_t scope)
{
   agent_t *ag = agent_route_with_caps(cfg, role, sys_cfg, required_caps, min_context);
   if (ag && scope != AGENT_SCOPE_UNSET && ag->max_scope != AGENT_SCOPE_UNSET &&
       ag->max_scope < scope)
      return NULL;
   return ag;
}
const char *agent_scope_name(agent_scope_t s)
{
   return s == AGENT_SCOPE_BOUNDED      ? "bounded"
          : s == AGENT_SCOPE_WHOLE_TASK ? "whole_task"
                                        : "unset";
}
agent_scope_t agent_scope_from_string(const char *s)
{
   if (s && strcmp(s, "bounded") == 0)
      return AGENT_SCOPE_BOUNDED;
   if (s && strcmp(s, "whole_task") == 0)
      return AGENT_SCOPE_WHOLE_TASK;
   return AGENT_SCOPE_UNSET;
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

/* Mirrors agent_config.c: the catalog (vendor) identity for capability lookup,
 * falling back to the wire provider when unset. Stubbed here because these tests
 * link delegate_routing.o without agent_config.o. */
const char *agent_catalog_provider(const agent_t *agent)
{
   if (!agent)
      return "";
   return agent->catalog_provider[0] ? agent->catalog_provider : agent->provider;
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
agent_route_block_t agent_routing_block_reason(const agent_t *agent, char *detail, size_t detail_sz)
{
   (void)agent;
   if (detail && detail_sz)
      detail[0] = '\0';
   return AGENT_ROUTE_OK;
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

int git_repo_root(const char *dir, char *out_root, size_t out_len)
{
   (void)dir;
   if (g_git_repo_root_rc == 0 && out_root && out_len > 0)
   {
      snprintf(out_root, out_len, "%s", g_git_repo_root_value);
      return 0;
   }
   if (out_root && out_len > 0)
      out_root[0] = '\0';
   return -1;
}
int worktree_sibling_path(const char *git_root, const char *sid, const char *work_name, char *out,
                          size_t out_len)
{
   (void)git_root;
   (void)sid;
   (void)work_name;
   if (g_worktree_sibling_path_rc == 0 && out && out_len > 0)
   {
      snprintf(out, out_len, "%s", g_worktree_sibling_path_value);
      return 0;
   }
   if (out && out_len > 0)
      out[0] = '\0';
   return -1;
}
int worktree_delegate_work_name(const char *sid, char *out, size_t cap)
{
   if (!sid || !out || cap < 9)
      return -1;
   snprintf(out, cap, "deadbeef"); /* stub: deterministic fixed name */
   return 0;
}
int is_aimee_worktree_path(const char *path)
{
   (void)path;
   return 0;
}
int worktree_managed_git_root(const char *path, char *out, size_t out_len)
{
   (void)path;
   if (out && out_len > 0)
      out[0] = '\0';
   return -1;
}
int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   (void)query;
   (void)project;
   (void)out;
   (void)max;
   return 0;
}

/* §7 graph-informed delegation pulls the structural blast radius of referenced
 * files into the delegate prompt; with no indexed project these no-op (the
 * injector then fails open and adds no block). */
int kb_client_index_list(project_info_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   (void)project;
   (void)file_path;
   (void)out;
   return -1;
}

int kb_client_index_find(const char *identifier, term_hit_t *out, int max)
{
   (void)identifier;
   (void)out;
   (void)max;
   return 0;
}
int worktree_create_sibling(const char *git_root, const char *sid, const char *work_name)
{
   (void)git_root;
   (void)sid;
   (void)work_name;
   g_worktree_create_calls++;
   return g_worktree_create_rc;
}

int worktree_create_sibling_from_anchor(const char *git_root, const char *sid,
                                        const char *work_name, const char *anchor_dir)
{
   (void)git_root;
   (void)sid;
   (void)work_name;
   (void)anchor_dir;
   g_worktree_create_calls++;
   return g_worktree_create_rc;
}

int worktree_create_sibling_on_branch(const char *git_root, const char *sid, const char *work_name,
                                      const char *branch, const char *anchor_dir)
{
   (void)git_root;
   (void)sid;
   (void)work_name;
   (void)branch;
   (void)anchor_dir;
   g_worktree_create_calls++;
   return g_worktree_create_rc;
}

void worktree_cleanup(const char *git_root, const char *sid, const char *work_name)
{
   (void)git_root;
   (void)sid;
   (void)work_name;
}

int worktree_apply_changes_to_parent(const char *src_wt, const char *dst_wt)
{
   g_worktree_apply_calls++;
   snprintf(g_last_apply_src, sizeof(g_last_apply_src), "%s", src_wt ? src_wt : "");
   snprintf(g_last_apply_dst, sizeof(g_last_apply_dst), "%s", dst_wt ? dst_wt : "");
   return 1;
}

int worktree_apply_delegate_changes_to_parent(const char *delegate_wt, const char *parent_wt,
                                              char *err, size_t err_len)
{
   g_delegate_apply_calls++;
   snprintf(g_last_apply_src, sizeof(g_last_apply_src), "%s", delegate_wt ? delegate_wt : "");
   snprintf(g_last_apply_dst, sizeof(g_last_apply_dst), "%s", parent_wt ? parent_wt : "");
   if (g_delegate_apply_rc < 0 && err && err_len > 0)
      snprintf(err, err_len, "stub delegate apply failed");
   return g_delegate_apply_rc;
}
int worktree_apply_delegate_changes_checked(const char *delegate_wt, const char *parent_hint,
                                            const char *launch_head, int *applied,
                                            char *parent_root, size_t parent_root_len, char *err,
                                            size_t err_len)
{
   (void)launch_head;
   char parent[MAX_PATH_LEN] = "";
   if (parent_hint && git_repo_root(parent_hint, parent, sizeof(parent)) != 0)
      snprintf(parent, sizeof(parent), "%s", parent_hint);
   int n = worktree_apply_delegate_changes_to_parent(delegate_wt, parent, err, err_len);
   if (applied)
      *applied = n;
   if (parent_root && parent_root_len > 0)
      snprintf(parent_root, parent_root_len, "%s", parent);
   return n < 0 ? -1 : 0;
}

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

int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg)
{
   (void)pool;
   g_submitted_fn = fn;
   g_submitted_arg = arg;
   return 0;
}

/* Coord/skill dispatch now runs delegates on-demand (server_delegate_ondemand.c,
 * not linked here). Capture instead of spawning a real thread, mirroring the
 * compute_pool_submit stub: the dispatched work is always delegate_worker. */
int delegate_spawn_ondemand(compute_ctx_t *cctx)
{
   g_submitted_fn = delegate_worker;
   g_submitted_arg = cctx;
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
   assert(cJSON_GetObjectItem(g_last_response, "participant") == NULL);
   db1_agent_job_t issued;
   assert(db1_agent_job_get(job_id, &issued) == 0);
   assert(strlen(issued.participant_token) == 64);
   db1_agent_job_t continued;
   assert(db1_agent_job_get_by_participant(issued.participant_token, &continued) == 0);
   assert(continued.id == job_id && strcmp(continued.agent_name, "codex") == 0);
   db1_agent_job_free(&continued);
   assert(db1_agent_job_get_by_participant(
              "0000000000000000000000000000000000000000000000000000000000000000", &continued) != 0);
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
   cJSON_AddStringToObject(req, "via", "codex");
   cJSON_AddTrueToObject(req, "background");

   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_last_response != NULL);
   cJSON *jid = cJSON_GetObjectItem(g_last_response, "job_id");
   assert(cJSON_IsNumber(jid));
   cJSON *participant = cJSON_GetObjectItem(g_last_response, "participant");
   assert(cJSON_IsString(participant) && strlen(participant->valuestring) == 64);
   int job_id = jid->valueint;
   assert(job_id > 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "pending") == 0);

   db1_agent_job_t job;
   assert(db1_agent_job_get(job_id, &job) == 0);
   assert(strcmp(job.status, "pending") == 0);
   assert(strcmp(job.role, "review") == 0);
   assert(strcmp(job.prompt, "run the background delegate test prompt") == 0);
   assert(strcmp(job.agent_name, "codex") == 0);

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

/* The SERVER's half of a refused launch: whatever the module says is what the
 * caller is told, verbatim, and nothing is written.
 *
 * This used to be two tests, each pinning one of the module's error strings --
 * "packet missing owned_files" and "missing handoff_schema delegate_result_v1".
 * Those are rules, and they are pinned where the rule now lives
 * (server-go/modules/delegates/launchplan_test.go). Keeping copies here would
 * have meant two places to update and one of them silently wrong. What could
 * only be tested here is that the wording survives the trip. */
static void test_delegate_launch_relays_the_modules_refusal(void)
{
   reset_last_response();
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   cJSON *req = cJSON_CreateObject();
   cJSON *plan = cJSON_AddObjectToObject(req, "plan");
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON_AddStringToObject(plan, "title", "Refused Launch Plan");
   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");
   add_launch_packet(packets, "packet-one", "Edit foo", "src/foo.c");

   g_launch_refusal = "delegate plan packet missing owned_files";
   assert(handle_delegate_launch(ctx, conn, req) == 0);
   g_launch_refusal = NULL;

   assert(g_last_response == NULL);
   assert(strcmp(g_last_error, "delegate plan packet missing owned_files") == 0);

   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_launch_relays_the_modules_refusal\n");
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
   /* `execute`, not `code`: these cases are about the handoff contract and the
    * turn loop, and any role that runs will show them. A `code` delegate holds
    * repo_write, and a write-capable delegate refuses a workspace that has no
    * checkout -- which is what this harness provides -- so it would never reach
    * the thing under test. `execute` also keeps the prompt small: the roles that
    * ask for parent-diff evidence arrive carrying the whole parent diff, which
    * pushes the contract past the prompt cap. The write path has its own cases
    * below. */
   cJSON_AddStringToObject(req, "role", "execute");
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
   cJSON_AddStringToObject(req, "role", "execute");
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
   cJSON_AddStringToObject(req, "role", "execute");
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

static void test_direct_delegate_tool_loop_cap_is_request_wide(void)
{
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "bounded delegate completed";
   g_last_request_tool_loop_cap = -999;
   g_last_request_tool_loop_deadline = -999;
   g_last_agent_tool_loop_cap = -999;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the request-wide timeout cap test");
   cJSON_AddTrueToObject(req, "tools");
   cJSON_AddNumberToObject(req, "tool_loop_timeout_ms_cap", 4321);
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker && g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[256];
   assert(read(fds[0], buf, sizeof(buf)) >= 0);
   close(fds[0]);
   assert(g_last_request_tool_loop_cap == 4321);
   assert(g_last_request_tool_loop_deadline > 0);
   assert(g_last_agent_tool_loop_cap == 4321);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_tool_loop_cap_is_request_wide\n");
}

/* Regression: an explicit tools:false (CLI --no-tools) on a tools-on-by-default
 * exec role must run the tools-OFF branch AND force tools off for the turn, so
 * agent_run_ex cannot silently re-enable tools from the agent config (the codex
 * "no content in final response" bug). */
static void test_direct_delegate_no_tools_forces_no_tools(void)
{
   reset_last_response();
   g_agent_run_calls = 0;
   g_agent_tool_run_calls = 0;
   g_force_no_tools = 0;
   g_agent_run_seen_no_tools = 0;
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "answer produced without tools";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "review");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the direct delegate no-tools policy test prompt");
   cJSON_AddFalseToObject(req, "tools");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   assert(g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0);
   buf[n] = '\0';
   close(fds[0]);
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   /* tools:false -> the no-tools branch (agent_run, not agent_run_with_tools)... */
   assert(g_agent_run_calls == 1);
   assert(g_agent_tool_run_calls == 0);
   /* ...and the fix: tools were force-disabled for the turn. */
   assert(g_agent_run_seen_no_tools == 1);
   /* the override is cleared again after the run. */
   assert(g_force_no_tools == 0);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_direct_delegate_no_tools_forces_no_tools\n");
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
   cJSON_AddStringToObject(req, "role", "execute");
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

/* A role an operator defined WITHOUT tools does not get them, even though the
 * role it is named after ships with them.
 *
 * This is the case the permission has to be resolved early for. The tool default
 * is decided when the request is handled and the mount is decided later; while
 * those were answered from different places, the early one could only see the
 * built-in table, so `code` was handed tools here and refused at dispatch. Both
 * now read the set resolved once when the role was validated.
 *
 * The recorded answer for this definition lives in delegate_permissions_stub.c;
 * nothing here decides what the block means. */
static void test_a_defined_role_without_tools_is_not_given_them(void)
{
   reset_last_response();
   g_role_definition = "permissions:\n  - knowledge_write\n";
   g_agent_run_calls = g_agent_tool_run_calls = 0;

   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "defined role ran";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the defined role tools test prompt");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker && g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0);
   close(fds[0]);

   /* The harness tells the two apart by which entry point ran: a delegate given
      tools goes through agent_run_with_tools, one without through agent_run. */
   assert(g_agent_tool_run_calls == 0);
   assert(g_agent_run_calls == 1);

   g_role_definition = NULL;
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_a_defined_role_without_tools_is_not_given_them\n");
}

/* A scoped `repo_write` only covers what it lists.
 *
 * The delegate below names no workspace at all, so nothing shows its target is
 * in scope and it runs read-only. It RUNS, which is the assertion: a
 * write-capable delegate cannot get through this harness, so reaching "done" is
 * how read-only shows up here.
 *
 * The object matched is the repository the CALLER named, because that is what an
 * operator means by a path in `scopes:` and the only thing that exists when the
 * decision is made. The recorded answer lives in delegate_permissions_stub.c. */
static void test_a_scoped_repo_write_does_not_cover_an_unnamed_workspace(void)
{
   reset_last_response();
   g_role_definition = "permissions:\n  - name: repo_write\n    scopes: [/srv/repo-a]\n";
   g_agent_run_calls = g_agent_tool_run_calls = 0;

   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "scoped role ran";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "run the scoped repo_write test prompt");
   /* No cwd, which is the other half of the rule: with a scoped grant and
      nothing naming the target, there is no way to show it is in scope, so the
      delegate is read-only. "Probably fine" is not a permission. */
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker && g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[2048];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0);
   close(fds[0]);

   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   /* It RAN. A write-capable delegate in this harness cannot: it refuses a
      workspace with no checkout. Read-only is the whole assertion. */
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);

   g_role_definition = NULL;
   run_cmd_set_cwd(NULL);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_a_scoped_repo_write_does_not_cover_an_unnamed_workspace\n");
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
   cJSON_AddStringToObject(req, "role", "execute");
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
static void test_delegate_launch_repairs_paths_from_request_cwd(void)
{
   /* An earlier case may have pointed HOME at its own tmpdir: re-register the
    * workspace roots, without which this delegation is refused outright. */
   register_test_workspace_root();
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
   /* An earlier case may have pointed HOME at its own tmpdir: re-register the
    * workspace roots, without which this delegation is refused outright. */
   register_test_workspace_root();
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "Validation complete — no findings";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", test_parent_repo());
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s",
            test_delegate_wt());
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
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
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
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
}

static void assert_role_gets_evidence_bundle(const char *role)
{
   /* The registered temp root, not a literal /tmp: the harness sets TMPDIR, and an
    * unregistered tree is refused rather than quietly run in-process. */
   assert_role_gets_evidence_bundle_with_cwd(role, platform_tmpdir());
}

static void test_read_only_delegate_uses_parent_workspace(void)
{
   /* An earlier case may have pointed HOME at its own tmpdir: re-register the
    * workspace roots, without which this delegation is refused outright. */
   register_test_workspace_root();
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "Read-only validation complete";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", test_parent_repo());
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s",
            test_delegate_wt());
   /* The workspace must EXIST: it is canonicalized before it can be authorized,
    * and an unresolvable tree is now a refusal rather than a quiet host run. */
   (void)mkdir(test_parent_repo(), 0700);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "validate");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "Read-only. Validate the changes in the current diff for correctness.");
   cJSON_AddStringToObject(req, "cwd", test_parent_repo());
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);

   char buf[4096];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);

   assert(g_worktree_create_calls == 0); /* read-only: no sibling worktree */
   assert(g_worktree_apply_calls == 0);
   assert(strstr(g_last_agent_prompt, "Validation Evidence Bundle") != NULL);
   /* (WP-B) async-only: read the persisted job rather than the connection. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_read_only_delegate_uses_parent_workspace\n");
}

static void test_provided_review_target_suppresses_worktree_evidence(void)
{
   /* An earlier case may have pointed HOME at its own tmpdir: re-register the
    * workspace roots, without which this delegation is refused outright. */
   register_test_workspace_root();
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "Plan review complete";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", test_parent_repo());

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "review");
   cJSON_AddStringToObject(req, "persona", "reviewer");
   cJSON_AddStringToObject(req, "prompt",
                           "BEGIN_ARTIFACT_DATA (plan)\nPLAN_TARGET_MARKER\nEND_ARTIFACT_DATA");
   cJSON_AddStringToObject(req, "cwd", test_parent_repo());
   cJSON_AddBoolToObject(req, "provided_target", 1);
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);
   char buf[256];
   assert(read(fds[0], buf, sizeof(buf)) >= 0);
   close(fds[0]);

   assert(strstr(g_last_agent_prompt,
                 "BEGIN_ARTIFACT_DATA (plan)\nPLAN_TARGET_MARKER\nEND_ARTIFACT_DATA") != NULL);
   assert(strstr(g_last_agent_prompt, "Validation Evidence Bundle") == NULL);
   assert(strstr(g_last_agent_prompt, "Parent Worktree Diff Evidence") == NULL);
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   assert(strcmp(job.status, "done") == 0);
   db1_agent_job_free(&job);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_provided_review_target_suppresses_worktree_evidence\n");
}

static void test_read_only_branch_delegate_rejected(void)
{
   /* An earlier case may have pointed HOME at its own tmpdir: re-register the
    * workspace roots, without which this delegation is refused outright. */
   register_test_workspace_root();
   reset_last_response();
   int fds[2];
   assert(pipe(fds) == 0);
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->fd = fds[1];
   g_agent_response = "should not run";
   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", test_parent_repo());
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s",
            test_delegate_wt());

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "validate");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "Read-only. Validate the changes in the current diff for correctness.");
   cJSON_AddStringToObject(req, "cwd", test_parent_repo());
   cJSON_AddStringToObject(req, "branch", "feature/read-only-review");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;
   close(fds[1]);

   char buf[4096];
   ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
   assert(n >= 0); /* (WP-B) async: response in g_last_response, pipe empty */
   buf[n] = '\0';
   close(fds[0]);

   assert(g_worktree_create_calls == 0);
   assert(g_agent_run_calls == 0); /* rejected before the agent ran */
   assert(g_agent_tool_run_calls == 0);
   /* (WP-B) async-only: the rejection error is persisted to the job row. */
   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
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
 * synchronously, then map the persisted job row back to the old {status,message}
 * vocabulary. Returns the synthetic response (caller frees) or NULL.
 *
 * (WP-B) async-only: handle_delegate returns the {job_id,"pending"} envelope via
 * the stubbed server_send_ok (captured in g_last_response, NOT written to the
 * connection — the pipe stays empty). The worker persists the real status/result
 * to the job row. We map done->ok, cancelled->cancelled, else->error so the
 * invariant cases (which mainly assert thread-local context restoration) still
 * read a status. */
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
   close(fds[0]);
   free(conn);
   free(ctx);
   cJSON *jid =
       g_last_response ? cJSON_GetObjectItemCaseSensitive(g_last_response, "job_id") : NULL;
   if (!cJSON_IsNumber(jid))
      return NULL;
   cJSON *out = cJSON_CreateObject();
   db1_agent_job_t job;
   if (db1_agent_job_get(jid->valueint, &job) == 0)
   {
      const char *st = strcmp(job.status, "done") == 0        ? "ok"
                       : strcmp(job.status, "cancelled") == 0 ? "cancelled"
                                                              : "error";
      cJSON_AddStringToObject(out, "status", st);
      if (job.result && job.result[0])
         cJSON_AddStringToObject(out, "message", job.result);
      db1_agent_job_free(&job);
   }
   return out;
}

/* A background delegate on a DETACHED (client-served) workspace has no live client
 * by the time its worker runs, so its shell is redirected into a server-side
 * ephemeral workspace that holds no checkout. Its FILE tools still resolve the
 * registered workspace, so a write-capable delegate there can edit the tree and
 * cannot build, test, or diff what it edited -- observed as a delegate truncating
 * a 2157-line file and reporting no error. That combination is refused; a
 * read-only delegate in the same position is not, because inspection with no
 * checkout is useless rather than destructive. */
static void bg_detached_delegate_case(const char *role, const char *prompt, int expect_refusal,
                                      int with_mirror_inputs)
{
   reset_last_response();
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-bgdetached-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");

   char ws[600];
   snprintf(ws, sizeof(ws), "%s/repo", tmpdir);
   char wt[760];
   snprintf(wt, sizeof(wt), "%s/.aimee/worktrees/bgdetached/main", ws);
   char mk[900];
   snprintf(mk, sizeof(mk), "mkdir -p %s", wt);
   assert(system(mk) == 0);

   /* Register the workspace by writing the config file directly. The config
    * struct is a secret of the config module, so this test never names it. */
   char cfgdir[700];
   snprintf(cfgdir, sizeof(cfgdir), "mkdir -p %s/.config/aimee", tmpdir);
   assert(system(cfgdir) == 0);
   char cfgpath[700];
   snprintf(cfgpath, sizeof(cfgpath), "%s/.config/aimee/aimee.yaml", tmpdir);
   /* A detached workspace that has been mirror-synced records a remote and a
    * head; the server can then reconstruct an equivalent tree with no client
    * present. A local repository stands in for the remote so this stays offline. */
   char remote[700] = "", head[64] = "";
   if (with_mirror_inputs)
   {
      snprintf(remote, sizeof(remote), "%s/origin", tmpdir);
      char gitcmd[1800];
      snprintf(gitcmd, sizeof(gitcmd),
               "git init -q %s && cd %s && git config user.email t@t && git config user.name t && "
               "echo seed > seed.txt && git add -A && git commit -q -m seed",
               remote, remote);
      assert(system(gitcmd) == 0);
      snprintf(gitcmd, sizeof(gitcmd), "git -C %s rev-parse HEAD", remote);
      FILE *rp = popen(gitcmd, "r");
      assert(rp != NULL);
      assert(fgets(head, sizeof(head), rp) != NULL);
      pclose(rp);
      head[strcspn(head, "\n")] = '\0';
      assert(head[0]);
   }
   FILE *cf = fopen(cfgpath, "w");
   assert(cf != NULL);
   fprintf(cf, "workspaces:\n  - path: %s\n    provider: detached\n", ws);
   if (with_mirror_inputs)
      fprintf(cf, "    remote: %s\n    head: %s\n", remote, head);
   fclose(cf);
   config_reload();
   assert(config_workspace_count() == 1);

   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", ws);
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s", wt);
   run_cmd_set_cwd(NULL);
   g_shell_root_during_run[0] = g_file_write_root[0] = g_last_root_notice[0] = '\0';
   g_agent_response = "did the work";
   g_agent_supports_code = 1;
   snprintf(g_agent_role, sizeof(g_agent_role), "%s", role);

   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", role);
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", prompt);
   cJSON_AddStringToObject(req, "cwd", ws);
   cJSON_AddStringToObject(req, "via", "test-agent");
   cJSON_AddTrueToObject(req, "tools");
   cJSON_AddTrueToObject(req, "background");
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker && g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;

   db1_agent_job_t job;
   assert(delegate_current_job(&job) == 0);
   const char *result = job.result ? job.result : "";
   int refused = strstr(result, "which contains no checkout") != NULL;
   if (refused != expect_refusal)
      fprintf(stderr, "  role=%s expected_refusal=%d got=%d status=%s result=[%s]\n", role,
              expect_refusal, refused, job.status, result);
   assert(refused == expect_refusal);
   if (expect_refusal)
   {
      /* The refusal must be actionable: name the ephemeral workspace and both
       * ways out, not merely decline. */
      assert(strstr(result, "delegate-ws") != NULL);
      assert(strstr(result, "aimee workspace serve") != NULL);
      /* and the server-side route that works without a client at all */
      assert(strstr(result, "--provider mirror") != NULL);
   }
   db1_agent_job_free(&job);

   g_agent_supports_code = 0;
   g_agent_role[0] = '\0';
   run_cmd_set_cwd(NULL);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
}

static void test_bg_detached_write_delegate_is_refused(void)
{
   /* Write intent: delegate_prompt_allows_writes() reads it from the prompt. */
   bg_detached_delegate_case(
       "code", "implement the fix: edit native_runner.go and write a clarifying sentence", 1, 0);
   printf("  PASS: test_bg_detached_write_delegate_is_refused\n");
}

/* A detached workspace that HAS been mirror-synced records a remote and a head,
 * so the server reconstructs an equivalent tree from its own bare mirror and runs
 * the delegate there instead of refusing it. That tree is the last synced state,
 * which the run announces; what matters here is that the delegate is no longer
 * stranded with nowhere to work. */
static void test_bg_detached_write_delegate_uses_mirror_when_synced(void)
{
   bg_detached_delegate_case(
       "code", "implement the fix: edit native_runner.go and write a clarifying sentence", 0, 1);
   printf("  PASS: test_bg_detached_write_delegate_uses_mirror_when_synced\n");
}

static void test_bg_detached_readonly_delegate_still_runs(void)
{
   bg_detached_delegate_case("validate",
                             "read-only: review the current diff for correctness and report", 0, 0);
   /* This delegate runs where the two roots genuinely DO diverge and that is
    * allowed: its shell is in a repo-less scratch dir while its file tools point
    * at the client workspace. Reading in the wrong place is useless rather than
    * destructive, so it is not refused -- but it must be told, in both respects,
    * rather than left to conclude the repository is empty. */
   assert(strstr(g_last_root_notice, "WARNING") != NULL);
   assert(strstr(g_last_root_notice, "ephemeral scratch directory") != NULL);
   assert(strstr(g_last_root_notice, "do not reconstruct it from memory") != NULL);
   printf("  PASS: test_bg_detached_readonly_delegate_still_runs\n");
}

/* ONE ROOT PER DELEGATE TURN.
 *
 * A delegate has two halves that must stand in the same place: its file tools,
 * which write under the root the parent-write guard permits, and its shell, which
 * runs wherever run_cmd() was pointed. When those disagree the delegate can edit
 * code and cannot build, test, or diff what it edited -- and nothing tells it so.
 * Observed on an appliance: a delegate asked to add one comment line to a
 * 2157-line file truncated it to 5 lines and reported success.
 *
 * Both roots are sampled DURING the run (delegate_worker clears them on the way
 * out) and compared. Asserting equality is the point: a test that only checks the
 * shell root would have passed throughout the period the two disagreed. */
static void assert_delegate_roots_agree(const char *what)
{
   if (strcmp(g_shell_root_during_run, g_file_write_root) != 0)
      fprintf(stderr, "  %s: shell root [%s] != file write root [%s]\n", what,
              g_shell_root_during_run, g_file_write_root);
   assert(g_shell_root_during_run[0]);
   assert(strcmp(g_shell_root_during_run, g_file_write_root) == 0);
}

/* A background write delegate is isolated in its own sibling worktree, so its
 * file tools write there. Its shell must run there too, not in the parent. */
static void test_delegate_shell_and_file_roots_agree(void)
{
   /* An earlier case may have pointed HOME at its own tmpdir: re-register the
    * workspace roots, without which this delegation is refused outright. */
   register_test_workspace_root();
   reset_last_response();
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-rootparity-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char parent[600], sibling[600];
   snprintf(parent, sizeof(parent), "%s/repo", tmpdir);
   snprintf(sibling, sizeof(sibling), "%s/repo-wt", tmpdir);
   char mk[1400];
   snprintf(mk, sizeof(mk), "mkdir -p %s %s", parent, sibling);
   assert(system(mk) == 0);

   g_git_repo_root_rc = 0;
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", parent);
   g_worktree_create_rc = 0;
   g_worktree_sibling_path_rc = 0;
   snprintf(g_worktree_sibling_path_value, sizeof(g_worktree_sibling_path_value), "%s", sibling);
   run_cmd_set_cwd(NULL);
   g_shell_root_during_run[0] = g_file_write_root[0] = g_last_root_notice[0] = '\0';
   g_agent_response = "did the work";
   g_agent_supports_code = 1;

   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "code");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "implement the fix: edit util.c and write the guard");
   cJSON_AddStringToObject(req, "cwd", parent);
   cJSON_AddStringToObject(req, "via", "test-agent");
   cJSON_AddTrueToObject(req, "tools");
   cJSON_AddTrueToObject(req, "background"); /* concurrent => its own worktree */
   assert(handle_delegate(ctx, conn, req) == 0);
   assert(g_submitted_fn == delegate_worker && g_submitted_arg != NULL);
   g_submitted_fn(g_submitted_arg);
   g_submitted_arg = NULL;

   /* The delegate was isolated into the sibling worktree, and BOTH halves of it
    * are there -- writes and shell alike. */
   assert(strcmp(g_file_write_root, sibling) == 0);
   assert_delegate_roots_agree("bg write delegate in its own worktree");
   assert(strcmp(g_shell_root_during_run, parent) != 0);
   /* And the delegate was TOLD where it is, by name -- it does not have to infer
    * its own location from whether a command happens to work. */
   assert(strstr(g_last_root_notice, sibling) != NULL);
   assert(strstr(g_last_root_notice, "WARNING") == NULL); /* nothing diverged */

   g_agent_supports_code = 0;
   run_cmd_set_cwd(NULL);
   cJSON_Delete(req);
   reset_last_response();
   free(conn);
   free(ctx);
   printf("  PASS: test_delegate_shell_and_file_roots_agree\n");
}

/* Same requirement on the path where the root is not the one the caller named:
 * a background delegate on a mirror-synced DETACHED workspace is moved to a
 * server-side reconstruction. Both halves must move together -- this is the case
 * that regressed, where the shell was redirected long after the worktree had
 * already been resolved against the client's path. */
static void test_bg_detached_mirror_delegate_roots_agree(void)
{
   bg_detached_delegate_case(
       "code", "implement the fix: edit native_runner.go and write a clarifying sentence", 0, 1);
   assert_delegate_roots_agree("bg write delegate on a mirror-synced detached workspace");
   /* Agreeing on a root is not enough here: the tree is a reconstruction at the
    * last synced head, so the delegate must be told that what it is looking at
    * can legitimately be behind the caller. */
   assert(strstr(g_last_root_notice, g_shell_root_during_run) != NULL);
   assert(strstr(g_last_root_notice, "mirror-sync") != NULL);
   assert(strstr(g_last_root_notice, "WARNING") == NULL);
   printf("  PASS: test_bg_detached_mirror_delegate_roots_agree\n");
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

/* (WP-B) async-only: a successful delegate persists to a durable, pollable job
 * row — status "done" with the provider's response text in job.result. The rich
 * synchronous envelope (delegation_id / turns / tool_calls / agent / token
 * counts) the worker once wrote over the connection is no longer delivered here;
 * `delegate.status` polling surfaces what the job row preserves. sci_drive_delegate
 * maps the row back to {status:"ok", message:<result>}. */
static void test_delegate_worker_ok_response_shape(void)
{
   reset_last_response();
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';

   g_agent_response = "the delegate result text";
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt", "response shape characterization");
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);

   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "ok") == 0);
   assert(strcmp(cJSON_GetObjectItem(resp, "message")->valuestring, "the delegate result text") ==
          0);
   cJSON_Delete(resp);

   reset_last_response();
   printf("  PASS: test_delegate_worker_ok_response_shape\n");
}

static void test_delegate_cost_cap_rejects_oversized_input_before_provider(void)
{
   reset_last_response();
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "role", "execute");
   cJSON_AddStringToObject(req, "persona", "engineer");
   cJSON_AddStringToObject(req, "prompt",
                           "This prompt plus the conservative provider framing cannot fit the "
                           "sub-token workflow cost allowance.");
   cJSON_AddNumberToObject(req, "max_cost_usd", 0.000000001);
   cJSON *resp = sci_drive_delegate(req);
   cJSON_Delete(req);
   assert(resp != NULL);
   assert(strcmp(cJSON_GetObjectItem(resp, "status")->valuestring, "error") == 0);
   assert(g_agent_run_calls == 0);
   assert(g_agent_tool_run_calls == 0);
   cJSON_Delete(resp);
   reset_last_response();
   printf("  PASS: test_delegate_cost_cap_rejects_oversized_input_before_provider\n");
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
   cJSON_AddStringToObject(req, "prompt", "error-path restore characterization");
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
   cJSON_AddStringToObject(req, "prompt", "session-during-run characterization");
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

/* test_server_compute_child_env.inc: delegate_child_export_context_env coverage.
 * Split out of test_server_compute.c to keep it under the 2000-line hard limit.
 * Included into test_server_compute.c so it shares the server_compute.c
 * translation unit (reaches tl_delegation_depth / tl_parent_delegation_id). */

/* delegate_child_export_context_env mirrors the forking thread's TLS depth/parent
 * into the env for a post-fork child. It must export the THREAD's values (not a
 * concurrently-clobbered global) when inside a delegate, and leave the env alone
 * for a primary agent (depth 0, no parent). */
static void test_child_export_context_env(void)
{
   /* Pre-seed the env with a stale value as if a concurrent delegate had written
    * it; inside a delegate the export must override with this thread's TLS. */
   platform_setenv("AIMEE_DELEGATE_DEPTH", "9");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "stale-parent");

   tl_delegation_depth = 3;
   snprintf(tl_parent_delegation_id, sizeof(tl_parent_delegation_id), "deleg-self");
   delegate_child_export_context_env();
   assert(strcmp(getenv("AIMEE_DELEGATE_DEPTH"), "3") == 0);
   assert(strcmp(getenv("AIMEE_PARENT_DELEGATION_ID"), "deleg-self") == 0);

   /* Primary agent: depth 0, no parent -> no override (inherited env preserved). */
   platform_setenv("AIMEE_DELEGATE_DEPTH", "7");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "inherited");
   tl_delegation_depth = 0;
   tl_parent_delegation_id[0] = '\0';
   delegate_child_export_context_env();
   assert(strcmp(getenv("AIMEE_DELEGATE_DEPTH"), "7") == 0);
   assert(strcmp(getenv("AIMEE_PARENT_DELEGATION_ID"), "inherited") == 0);

   platform_setenv("AIMEE_DELEGATE_DEPTH", "");
   platform_setenv("AIMEE_PARENT_DELEGATION_ID", "");
}

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

   /* WP-C.2c(3): a conn with NO principal falls back to the per-turn thread-local
    * (set by the chat worker) so a chat-spawned delegate inherits the user's
    * principal. */
   agent_set_request_vault_principal("webuser:carol");
   server_conn_t bare2;
   memset(&bare2, 0, sizeof(bare2));
   bare2.fd = -1;
   cJSON *req3 = cJSON_CreateObject();
   compute_ctx_t *c3 = create_compute_ctx(NULL, &bare2, req3);
   assert(c3 != NULL);
   assert(strcmp(c3->vault_principal, "webuser:carol") == 0);
   compute_ctx_free(c3);
   cJSON_Delete(req3);

   /* The conn principal still WINS when present (the thread-local doesn't
    * override an attested conn). */
   server_conn_t named;
   memset(&named, 0, sizeof(named));
   named.fd = -1;
   named.attested_transport = ATTEST_UDS_PEERCRED;
   snprintf(named.vault_principal, sizeof(named.vault_principal), "uid:1000");
   cJSON *req4 = cJSON_CreateObject();
   compute_ctx_t *c4 = create_compute_ctx(NULL, &named, req4);
   assert(c4 != NULL);
   assert(strcmp(c4->vault_principal, "uid:1000") == 0); /* conn wins over thread-local */
   compute_ctx_free(c4);
   cJSON_Delete(req4);
   agent_set_request_vault_principal(NULL); /* clear for other tests */
   printf("  PASS: test_create_compute_ctx_threads_vault_identity\n");
}

/* P1 (cred-vault-consolidation): the codex-oauth override prefers the attested
 * per-turn principal, falls back to the server-owned vault for a TCP thin client
 * with no per-turn principal, and treats a LOCKED per-turn cred as a hard miss
 * (D15 — never silently downgraded to the server vault). */
static void test_codex_oauth_vault_server_principal_fallback(void)
{
   agent_t ag;
   memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "codex");
   snprintf(ag.auth_type, sizeof(ag.auth_type), "codex-oauth");
   ag.credential_count = 0;
   char lp[160] = "", lc[64] = "", csp[512] = "";
   int cd = 0;

   /* (a) no per-turn principal; the server-owned vault has the token -> applied. */
   codex_fixture_reset();
   g_codex_srv_status = VAULT_OK;
   g_codex_srv_token = "srv-codex-token";
   delegate_resolve_credentials("", &ag, lp, sizeof lp, lc, sizeof lc, csp, sizeof csp, &cd);
   assert(g_codex_set_called == 1);
   assert(strcmp(g_codex_set_token, "srv-codex-token") == 0);

   /* (b) There is no per-turn principal to be locked out of: the credential is
    * the environment's, so a locked per-user vault is not a second source that
    * could be escalated past. D15 guarded a fallback BETWEEN principals; with one
    * principal the environment credential simply applies. */
   codex_fixture_reset();
   g_codex_perturn_status = VAULT_ERR_LOCKED;
   g_codex_srv_status = VAULT_OK;
   g_codex_srv_token = "srv-codex-token";
   delegate_resolve_credentials("uid:1000", &ag, lp, sizeof lp, lc, sizeof lc, csp, sizeof csp,
                                &cd);
   assert(g_codex_set_called == 1);
   assert(strcmp(g_codex_set_token, "srv-codex-token") == 0);

   /* (c) both miss -> no creds applied. */
   codex_fixture_reset();
   delegate_resolve_credentials("uid:1000", &ag, lp, sizeof lp, lc, sizeof lc, csp, sizeof csp,
                                &cd);
   assert(g_codex_set_called == 0);

   codex_fixture_reset();
   printf("PASS: codex oauth vault server-principal fallback + D15 locked hard-miss\n");
}

/* Judging a handoff is the delegates module's rule now
 * (server-go/modules/delegates/handoff.go) and this binary hosts no bus. The
 * subject of the three handoff tests here is COMPUTE'S REPAIR LOOP -- a valid
 * handoff costs one agent call, a malformed one is repaired and retried, and a
 * second malformed one is an error -- so the test states the one thing its
 * fixtures vary: whether the agent's text is a well-formed handoff at all.
 *
 * This is deliberately NOT the rule. It does not check status admission, the
 * required arrays, summary presence, ownership or the done-without-verification
 * downgrade, so it cannot drift into a second copy of a rule that lives in
 * exactly one place. */
static int compute_test_handoff_provider(const char *text, const char *owned_files_json,
                                         int require_verification,
                                         delegate_handoff_validation_t *out)
{
   (void)owned_files_json;
   (void)require_verification;
   memset(out, 0, sizeof(*out));
   snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");

   cJSON *root = text ? cJSON_Parse(text) : NULL;
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
   if (!cJSON_IsObject(root) || !cJSON_IsString(schema) ||
       strcmp(schema->valuestring, "delegate_result_v1") != 0)
   {
      cJSON_Delete(root);
      snprintf(out->error, sizeof(out->error), "%s", "handoff is not valid JSON object");
      out->needs_supervisor_review = 1;
      return -1;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   if (cJSON_IsString(status))
   {
      snprintf(out->raw_status, sizeof(out->raw_status), "%s", status->valuestring);
      snprintf(out->status, sizeof(out->status), "%s", status->valuestring);
   }
   cJSON_Delete(root);
   out->valid = 1;
   out->passed_tests = 1;
   return 0;
}

/* The route filter, as the module answers it.
 *
 * These cases are about compute and dispatch behaviour, not about capability
 * routing, so the fleet is small and every candidate qualifies. The decode side
 * is exercised for real, which is what keeps the harness honest about the wire
 * shape even though the policy is tested against the module. */
static int compute_test_route_filter(const uint8_t *request, size_t request_len, uint8_t *response,
                                     size_t response_cap, size_t *response_len)
{
   if (request_len < AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN)
      return -1;
   unsigned count = aimee_delegates_get_u32(request + 8);
   size_t need = 16u + (size_t)count * 4u;
   if (response_cap < need)
      return -1;

   memset(response, 0, need);
   aimee_delegates_put_u32(response, AIMEE_DELEGATES_ROUTEFILTER_RESPONSE_MAGIC);
   unsigned kept = 0;
   for (unsigned i = 0; i < count; i++)
   {
      const uint8_t *at = request + AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN +
                          (size_t)i * AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN;
      unsigned flags = aimee_delegates_get_u32(at);
      int candidate = (flags & AIMEE_DELEGATES_RF_ENABLED) && (flags & AIMEE_DELEGATES_RF_HAS_ROLE);
      if (candidate)
      {
         aimee_delegates_put_u32(response + 16 + i * 4, 1u);
         kept++;
      }
   }
   aimee_delegates_put_u32(response + 4, kept);
   aimee_delegates_put_u32(response + 12, aimee_delegates_get_u32(request + 12));
   *response_len = need;
   return 0;
}

/* A launch-plan provider that mirrors the wire's SHAPE, not its policy.
 *
 * These tests are about what the SERVER does with a launch -- that a plan row
 * and a coord job appear, with the right task count and concurrency, and that
 * the response says so. What makes a packet launchable, how a missing path is
 * repaired, and what a delegate is briefed with are the module's rules, and
 * they are tested against the module (launchplan_test.go). Restating them here
 * would be a second copy of exactly the kind this migration removes.
 *
 * So this reads the packets and emits one step and one task per non-review
 * packet. It mirrors exactly ONE branch of the module's rule -- a path that
 * does not exist and has exactly one candidate becomes that candidate -- because
 * one fixture below is about the request's `cwd` reaching the candidate lookup,
 * and that lookup is now the CALLER's half. Nothing else here decides anything. */
static int compute_test_launch_plan(const uint8_t *request, size_t request_len, uint8_t *response,
                                    size_t response_cap, size_t *response_len)
{
   aimee_delegates_rd_t r = {.buf = request, .len = request_len, .at = 0, .bad = 0};
   if (aimee_delegates_rd_u32(&r) != AIMEE_DELEGATES_LAUNCHPLAN_REQUEST_MAGIC ||
       aimee_delegates_rd_u32(&r) != (uint32_t)AIMEE_DELEGATES_WIRE_VERSION)
      return -1;
   int max_concurrent = (int)aimee_delegates_rd_u32(&r);

   char scratch[1024];
   aimee_delegates_rd_str(&r, scratch, sizeof(scratch)); /* schema */
   aimee_delegates_rd_str(&r, scratch, sizeof(scratch)); /* title */

   uint32_t missing = aimee_delegates_rd_u32(&r);
   for (uint32_t i = 0; i < missing && !r.bad; i++)
      aimee_delegates_rd_str(&r, scratch, sizeof(scratch));

   /* Collect the launchable packets before writing anything: the response
    * states its step count up front. */
   struct
   {
      char id[256];
      char title[256];
      char objective[512];
      char role[64];
      char files[8][512];
      uint32_t file_count;
   } launchable[64];
   uint32_t launch_count = 0;

   uint32_t packets = aimee_delegates_rd_u32(&r);
   for (uint32_t i = 0; i < packets && !r.bad; i++)
   {
      char id[256], title[256], objective[512], role[64], schema[128];
      aimee_delegates_rd_str(&r, id, sizeof(id));
      aimee_delegates_rd_str(&r, title, sizeof(title));
      aimee_delegates_rd_str(&r, objective, sizeof(objective));
      aimee_delegates_rd_str(&r, role, sizeof(role));
      aimee_delegates_rd_str(&r, schema, sizeof(schema));

      char paths[8][512];
      uint32_t kept = 0;
      uint32_t file_count = aimee_delegates_rd_u32(&r);
      for (uint32_t f = 0; f < file_count && !r.bad; f++)
      {
         char path[512];
         aimee_delegates_rd_str(&r, path, sizeof(path));
         int exists = aimee_delegates_rd_u32(&r) != 0;
         uint32_t candidates = aimee_delegates_rd_u32(&r);
         char sole_candidate[512] = "";
         for (uint32_t c = 0; c < candidates && !r.bad; c++)
         {
            char candidate[512];
            aimee_delegates_rd_str(&r, candidate, sizeof(candidate));
            if (candidates == 1 && !exists)
               snprintf(sole_candidate, sizeof(sole_candidate), "%s", candidate);
         }
         if (kept < 8)
            snprintf(paths[kept++], sizeof(paths[0]), "%s",
                     sole_candidate[0] ? sole_candidate : path);
      }

      if (strcmp(role, "review") == 0 || kept == 0 || launch_count >= 64)
         continue;
      snprintf(launchable[launch_count].id, sizeof(launchable[0].id), "%s", id);
      snprintf(launchable[launch_count].title, sizeof(launchable[0].title), "%s", title);
      snprintf(launchable[launch_count].objective, sizeof(launchable[0].objective), "%s",
               objective);
      snprintf(launchable[launch_count].role, sizeof(launchable[0].role), "%s",
               role[0] ? role : "execute");
      for (uint32_t f = 0; f < kept; f++)
         snprintf(launchable[launch_count].files[f], sizeof(launchable[0].files[0]), "%s",
                  paths[f]);
      launchable[launch_count].file_count = kept;
      launch_count++;
   }
   if (r.bad)
      return -1;

   aimee_delegates_wire_t w;
   aimee_delegates_wire_init(&w, response, response_cap);
   aimee_delegates_wire_u32(&w, AIMEE_DELEGATES_LAUNCHPLAN_RESPONSE_MAGIC);
   if (g_launch_refusal)
   {
      aimee_delegates_wire_str(&w, g_launch_refusal);
      launch_count = 0;
   }
   else
      aimee_delegates_wire_str(&w,
                               launch_count ? "" : "delegate plan has no implementation packets");
   aimee_delegates_wire_u32(&w, (uint32_t)(max_concurrent > 0 ? max_concurrent : 3));

   aimee_delegates_wire_u32(&w, launch_count);
   for (uint32_t i = 0; i < launch_count; i++)
   {
      aimee_delegates_wire_str(&w, launchable[i].title);
      aimee_delegates_wire_str(&w, launchable[i].id);
      aimee_delegates_wire_str(&w, launchable[i].objective);
      aimee_delegates_wire_str(&w, "");
   }

   aimee_delegates_wire_u32(&w, launch_count);
   for (uint32_t i = 0; i < launch_count; i++)
   {
      aimee_delegates_wire_u32(&w, launchable[i].file_count);
      for (uint32_t f = 0; f < launchable[i].file_count; f++)
         aimee_delegates_wire_str(&w, launchable[i].files[f]);
      aimee_delegates_wire_str(&w, launchable[i].role);
      aimee_delegates_wire_str(&w, launchable[i].objective);
   }

   aimee_delegates_wire_u32(&w, 0); /* repairs */
   aimee_delegates_wire_u32(&w, 0); /* warnings */

   if (w.overflow)
      return -1;
   *response_len = w.len;
   return 0;
}

int main(void)
{
   register_test_workspace_root();
   delegate_permissions_stub_install();
   delegate_register_launch_plan_provider(compute_test_launch_plan);
   delegate_register_route_filter_provider(compute_test_route_filter);
   assert(delegate_backend_register(&g_fake_docker) == 0);
   delegate_register_handoff_provider(compute_test_handoff_provider);
   test_codex_oauth_vault_server_principal_fallback();
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
   test_child_export_context_env();
   test_delegation_augment_error();
   test_cross_process_depth_propagation();
   test_delegate_provider_route_override();
   test_delegate_generated_ids_are_unique();
   test_delegate_status_handler();
   test_delegate_background_handler();
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
   test_delegate_launch_relays_the_modules_refusal();
   test_direct_delegate_handoff_json_response();
   test_direct_delegate_handoff_repair_attempt();
   test_direct_delegate_handoff_repair_failure_is_error();
   test_direct_delegate_review_auto_tools_uses_tools();
   test_direct_delegate_reviewer_alias_auto_tools_uses_tools();
   test_direct_delegate_explicit_tools_forces_tools();
   test_direct_delegate_tool_loop_cap_is_request_wide();
   test_direct_delegate_no_tools_forces_no_tools();
   test_direct_delegate_one_turn_diagnose_suppresses_default_tools();
   test_a_defined_role_without_tools_is_not_given_them();
   test_a_scoped_repo_write_does_not_cover_an_unnamed_workspace();
   test_direct_delegate_max_turns_override();
   test_read_only_delegate_uses_parent_workspace();
   test_provided_review_target_suppresses_worktree_evidence();
   test_read_only_branch_delegate_rejected();
   test_inspection_roles_get_evidence_bundle();
   test_delegate_worker_restores_caller_context();
   test_delegate_worker_ok_response_shape();
   test_delegate_cost_cap_rejects_oversized_input_before_provider();
   test_delegate_worker_restores_state_on_error();
   test_delegate_worker_sets_session_override_during_run();
   test_create_compute_ctx_threads_vault_identity();
   test_bg_detached_write_delegate_is_refused();
   test_bg_detached_readonly_delegate_still_runs();
   test_delegate_shell_and_file_roots_agree();
   test_bg_detached_mirror_delegate_roots_agree();
   test_bg_detached_write_delegate_uses_mirror_when_synced();
   db1_shutdown();
   reset_last_response();
   printf("server_compute: all tests passed\n");
   return 0;
}
