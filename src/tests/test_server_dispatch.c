#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "aimee.h"
#include "../db1/db.h"
#include "../db1/eval.h"
#include "../db1/server_sessions.h"
#include "kb_client.h"       /* kb_health_t for the stub below */
#include "server_internal.h" /* server_health_add_kb, for the kb verdict tests */
#include "agent_config.h"
#include "config_fields.h" /* config_field_lookup / _set_value for the config_set stub */
#include "agent_eval.h"
#include "hud.h"
#include "log.h"
#include "server.h"
#include "server_http.h"
#include "toolset.h"
#include "runtime_secret.h"
#include "vault_config_bootstrap.h"
#include "platform_ipc.h"
#include "platform_process.h"

int hud_gather(hud_status_t *out)
{
   memset(out, 0, sizeof(*out));
   return 0;
}

static char g_git_repo_root_prefix[MAX_PATH_LEN];
static char g_git_repo_root_value[MAX_PATH_LEN];

/* Worktree GC stubs — server.c calls into worktree_gc + git_repo_root for
 * `worktree.gc` and the auto-GC pass in hooks.session_start. The dispatch
 * test exercises method routing only; pretend every cwd is outside a git
 * repo by default so the GC paths short-circuit. */
#include "../worktree_gc.h"
int git_repo_root(const char *dir, char *out, size_t cap)
{
   if (dir && g_git_repo_root_prefix[0] && g_git_repo_root_value[0])
   {
      size_t len = strlen(g_git_repo_root_prefix);
      if (strncmp(dir, g_git_repo_root_prefix, len) == 0 && (dir[len] == '/' || dir[len] == '\0'))
      {
         snprintf(out, cap, "%s", g_git_repo_root_value);
         return 0;
      }
   }
   if (out && cap > 0)
      out[0] = '\0';
   return -1;
}
void worktree_gc_options_init(worktree_gc_options_t *opts)
{
   if (opts)
      memset(opts, 0, sizeof(*opts));
}
int worktree_gc_scan(const char *git_root, const worktree_gc_options_t *opts,
                     worktree_gc_candidate_t *out, int max_out)
{
   (void)git_root;
   (void)opts;
   (void)out;
   (void)max_out;
   return 0;
}
int worktree_gc_apply(const char *git_root, const worktree_gc_candidate_t *cands, int n,
                      const worktree_gc_options_t *opts)
{
   (void)git_root;
   (void)cands;
   (void)n;
   (void)opts;
   return 0;
}

char *hud_json(const hud_status_t *s)
{
   (void)s;
   return strdup("{}");
}

static const char *g_last_handler = NULL;
static char g_last_exec_cmd[1024];
static int g_exec_timeout_ms = 0;
static session_state_t g_saved_state;
static int g_session_state_save_calls = 0;
static session_state_t g_pre_tool_state;
static int g_pre_tool_seen_state = 0;
static int g_config_stateful = 0;
static int g_config_reload_calls = 0;
static int g_config_secret_store_calls = 0;
static int g_config_secret_store_configured = 0;
static config_t g_config_disk;
static config_t g_config_snapshot;

static char *read_all(int fd)
{
   char buf[8192];
   size_t used = 0;
   char *out = calloc(1, sizeof(buf));
   assert(out != NULL);
   ssize_t n;
   while ((n = read(fd, buf + used, sizeof(buf) - 1 - used)) > 0)
      used += (size_t)n;
   memcpy(out, buf, used);
   out[used] = '\0';
   return out;
}

static cJSON *dispatch_json(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t len)
{
   int fds[2];
   assert(pipe(fds) == 0);
   conn->fd = fds[1];
   assert(server_dispatch(ctx, conn, msg, len) == 0);
   close(fds[1]);
   char *resp = read_all(fds[0]);
   close(fds[0]);
   cJSON *json = cJSON_Parse(resp);
   assert(json != NULL);
   free(resp);
   return json;
}

static int stub_handler(server_conn_t *conn, const char *name)
{
   g_last_handler = name;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "route", name);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int platform_evloop_del(platform_evloop_t *loop, int fd)
{
   (void)loop;
   (void)fd;
   return 0;
}

/* Instrumented so test_conn_update_events_null_evloop can assert the invariant
 * that conn_update_events never hands a NULL loop to the event layer (a NULL
 * loop here means epoll_ctl(loop->epoll_fd,...) would dereference NULL and
 * crash the whole server — the /v1 HTTP-worker crash-loop regression). */
int g_evloop_mod_null_loop = 0;
int g_evloop_mod_nonnull_loop = 0;
int platform_evloop_mod(platform_evloop_t *loop, int fd, uint32_t events)
{
   if (loop)
      g_evloop_mod_nonnull_loop = 1;
   else
      g_evloop_mod_null_loop = 1;
   (void)fd;
   (void)events;
   return 0;
}

int platform_evloop_add(platform_evloop_t *loop, int fd, uint32_t events)
{
   (void)loop;
   (void)fd;
   (void)events;
   return 0;
}

int platform_evloop_create(platform_evloop_t *loop)
{
   (void)loop;
   return 0;
}

int platform_evloop_wait(platform_evloop_t *loop, platform_event_t *events, int max_events,
                         int timeout_ms)
{
   (void)loop;
   (void)events;
   (void)max_events;
   (void)timeout_ms;
   return 0;
}

void platform_evloop_destroy(platform_evloop_t *loop)
{
   (void)loop;
}

int platform_ipc_accept(int listen_fd)
{
   (void)listen_fd;
   return -1;
}

int platform_ipc_peer_cred(int fd, platform_peer_cred_t *out)
{
   (void)fd;
   if (out)
   {
      out->uid = 0;
      out->gid = 0;
      out->pid = 0;
   }
   return 0;
}

int platform_ipc_listen(const char *path, int backlog)
{
   (void)path;
   (void)backlog;
   return -1;
}

int platform_ipc_probe(const char *path)
{
   (void)path;
   return 0;
}

unsigned int platform_getuid(void)
{
   return 0;
}

int platform_get_exe_path(char *buf, size_t size)
{
   snprintf(buf, size, "/tmp/aimee-server");
   return 0;
}

int platform_exec_capture(const char *cmd, char **out, size_t *out_len, int timeout_ms)
{
   snprintf(g_last_exec_cmd, sizeof(g_last_exec_cmd), "%s", cmd ? cmd : "");
   g_exec_timeout_ms = timeout_ms;
   const char *json = "{\"status\":\"ok\",\"knowledge_ready\":true}";
   *out = strdup(json);
   assert(*out != NULL);
   if (out_len)
      *out_len = strlen(*out);
   return 0;
}

void db1_apply_pragmas(sqlite3 *db, db_mode_t mode)
{
   (void)db;
   (void)mode;
}

int compute_pool_init(compute_pool_t *pool, int num_threads)
{
   (void)pool;
   (void)num_threads;
   return 0;
}

void compute_pool_shutdown(compute_pool_t *pool)
{
   (void)pool;
}

/* On-demand delegate execution (server_delegate_ondemand.c) is not linked here;
 * server.c calls these at init/shutdown. No-op stubs. */
void delegate_ondemand_set_ceiling(int ceiling)
{
   (void)ceiling;
}

void delegate_ondemand_drain(int timeout_ms)
{
   (void)timeout_ms;
}

int server_session_pool_submit(server_ctx_t *ctx, const char *session_id, void (*fn)(void *),
                               void *arg, int *thread_count_out)
{
   (void)ctx;
   (void)session_id;
   if (thread_count_out)
      *thread_count_out = CONFIG_DEFAULT_SESSION_THREADS;
   if (fn)
      fn(arg);
   return 0;
}

void server_session_pool_close(server_ctx_t *ctx, const char *session_id)
{
   (void)ctx;
   (void)session_id;
}

void server_session_pools_shutdown(server_ctx_t *ctx)
{
   (void)ctx;
}

char *server_session_pools_json(server_ctx_t *ctx)
{
   (void)ctx;
   return strdup("[]");
}

int agent_eval_ablation_preset(const char *preset, agent_ablation_flags_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->configured = 1;
   return (preset && strcmp(preset, "invalid") == 0) ? -1 : 0;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->agent_count = 1;
   return 0;
}

int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks)
{
   (void)suite_dir;
   if (!tasks || max_tasks <= 0)
      return 0;
   snprintf(tasks[0].name, sizeof(tasks[0].name), "dispatch");
   return 1;
}

int agent_eval_run_with_options(agent_config_t *cfg, const char *suite_dir,
                                const agent_eval_run_options_t *options, eval_result_t *results,
                                int max_results)
{
   (void)cfg;
   (void)suite_dir;
   (void)options;
   if (results && max_results > 0)
   {
      snprintf(results[0].task_name, sizeof(results[0].task_name), "dispatch");
      snprintf(results[0].agent_name, sizeof(results[0].agent_name), "test-agent");
      snprintf(results[0].ablation, sizeof(results[0].ablation), "full");
      results[0].success = 1;
      results[0].tool_calls = 3;
      results[0].tool_call_failures = 1;
      results[0].rescue_recoveries = 2;
   }
   return 1;
}

int db1_eval_results_list(const char *suite_or_null, db1_eval_display_row_t *out, int max)
{
   (void)suite_or_null;
   if (!out || max <= 0)
      return 0;
   snprintf(out[0].suite, sizeof(out[0].suite), "delegate");
   snprintf(out[0].task_name, sizeof(out[0].task_name), "dispatch");
   snprintf(out[0].agent_name, sizeof(out[0].agent_name), "test-agent");
   snprintf(out[0].ablation, sizeof(out[0].ablation), "full");
   out[0].success = 1;
   out[0].tool_calls = 3;
   out[0].tool_call_failures = 1;
   out[0].rescue_recoveries = 2;
   out[0].latency_ms = 11;
   snprintf(out[0].created_at, sizeof(out[0].created_at), "2026-05-25T00:00:00Z");
   return 1;
}

int server_load_token(server_ctx_t *ctx)
{
   (void)ctx;
   return 0;
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

void audit_log(const char *event_type, const char *fmt, ...)
{
   (void)event_type;
   (void)fmt;
}

uint32_t server_capability_for_method(const char *method)
{
   if (strcmp(method, "tool.execute") == 0)
      return CAP_TOOL_EXECUTE;
   return 0;
}

const method_policy_t *server_policy_for_method(const char *method)
{
   static const method_policy_t policy = {"tool.execute", CAP_TOOL_EXECUTE, "execute tool"};
   return strcmp(method, "tool.execute") == 0 ? &policy : NULL;
}

int handle_auth(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "auth");
}
int handle_audit_verify(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "audit.verify");
}
int handle_audit_checkpoint(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "audit.checkpoint");
}
int handle_audit_seal(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "audit.seal");
}
int handle_audit_snapshot(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "audit.snapshot");
}
int handle_audit_captures(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "audit.captures");
}
int handle_audit_replay(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "audit.replay");
}

/* hooks.session_start invokes session_start_emit (cmd_session_lifecycle.c)
 * which the test does not link. Stub it and the session-id thread-local so
 * the dispatch table builds cleanly. */
void session_start_emit(app_ctx_t *ctx, const char *hook_input, FILE *out)
{
   (void)ctx;
   (void)hook_input;
   (void)out;
}
/* handle_hooks_session_start now registers a server_sessions row; stub the two
 * db1 accessors it uses so this dispatch-routing test need not link the db1
 * layer. get -> miss (so create is attempted), create -> success. */
int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   (void)id;
   (void)out;
   return -1;
}
int db1_server_session_create(const char *id, const char *client_type, const char *principal)
{
   (void)id;
   (void)client_type;
   (void)principal;
   return 0;
}
/* session.brief_assemble invokes session_brief_emit (cmd_session_lifecycle.c),
 * also not linked here. Stub it with a marker so the op handler test can assert
 * the emitted brief flows into the response envelope. */
void session_brief_emit(FILE *out)
{
   if (out)
      fputs("STUB_BRIEF_CONTENT", out);
}
static int g_memory_scope_begin_calls = 0;
static int g_memory_scope_clear_calls = 0;
int server_memory_scope_begin(cJSON *req)
{
   (void)req;
   g_memory_scope_begin_calls++;
   return 0;
}
void kb_client_memory_scope_context_clear(void)
{
   g_memory_scope_clear_calls++;
}
/* memory.user_capture invokes db1_user_memory_upsert (db1/user_memory.c), not
 * linked here. Stub it so the dispatch table builds. */
int db1_user_memory_upsert(const char *kind, const char *tier, const char *key, const char *content,
                           double confidence, const char *source_session)
{
   (void)kind;
   (void)tier;
   (void)key;
   (void)content;
   (void)confidence;
   (void)source_session;
   return 0;
}
void session_id_set_override(const char *sid)
{
   (void)sid;
}
void session_id_clear_override(void)
{
}
int handle_session_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.create");
}
int handle_session_record_transcript(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.record_transcript");
}
int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.list");
}
int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.get");
}
int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.close");
}
int handle_chat_graceful_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "chat.graceful_cancel");
}
int handle_chat_interrupt(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "chat.interrupt");
}
int handle_session_brief(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.brief");
}
int handle_session_attach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.attach");
}
int handle_session_detach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.detach");
}
int handle_session_presence(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "session.presence");
}
/* The roundtable pipeline handlers are STUBBED here (this test only exercises
 * dispatch-table routing) so the real server_pipeline.o — which drags in the
 * op-run/git/agent-config/chunk graph — is not linked into the dispatch test. */
int handle_pipeline_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.start");
}
int handle_pipeline_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.status");
}
int handle_pipeline_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.list");
}
int handle_pipeline_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.cancel");
}
int handle_pipeline_resume(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.resume");
}
int handle_pipeline_advance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.advance");
}
int handle_pipeline_gate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "pipeline.gate");
}
int handle_trajectory_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "trajectory.export");
}
int handle_trajectory_batch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "trajectory.batch");
}
int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.search");
}
int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.store");
}
int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.list");
}
int handle_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.stats");
}
int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.get");
}

int handle_memory_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.delete");
}

int handle_memory_supersede(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.supersede");
}
int handle_memory_read(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.read");
}
int handle_memory_benchmark(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.benchmark");
}
int handle_index_scan(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.scan");
}
int handle_index_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.ingest");
}
int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.find");
}
int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.list");
}
int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.blast_radius");
}
int handle_index_structure(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.structure");
}
int handle_index_span(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.span");
}
int handle_index_hybrid(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.hybrid");
}
int handle_index_investigate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.investigate");
}
int handle_index_find_callers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.find_callers");
}
int handle_index_deps(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "index.deps");
}
int handle_repo_trust(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "repo.trust");
}
int handle_graph_sync_code(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "graph.sync_code");
}
int handle_graph_explain(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "graph.explain");
}
int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "blast_radius.preview");
}
int handle_kb_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.search");
}
int handle_evidence_trace(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "evidence.trace_retrieval_event");
}
int handle_evidence_provenance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "evidence.provenance_retrieval_event");
}
int handle_evidence_fidelity(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "evidence.fidelity_retrieval_event");
}
int handle_css_signals(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "css.signals");
}
int handle_curator_implements(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "curator.implements");
}
int handle_curator_synthesize(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "curator.synthesize");
}
int handle_curator_contradictions(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "curator.contradictions");
}
int handle_curator_invalidated(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "curator.invalidated");
}
/* server_api_status.c (linked here for handle_api_*) probes the kb from
 * server.health. This test exercises the dispatch table, not the kb, so answer
 * "unreachable" by default without linking the whole kb client.
 *
 * The health-verdict tests below drive it instead: set g_kb_health_rc and the
 * fields they care about, so server_health_add_kb's aggregation can be exercised
 * without a kb. */
static int g_kb_health_rc = -1;
static kb_health_t g_kb_health;

int kb_client_health(kb_health_t *out)
{
   if (out)
      *out = g_kb_health;
   return g_kb_health_rc;
}

static void kb_health_stub_reset(void)
{
   memset(&g_kb_health, 0, sizeof(g_kb_health));
   g_kb_health_rc = -1;
}

/* server.health also reports the kb transport breaker, so an operator can see
 * that calls are being refused locally while the kb itself looks fine. Same
 * reasoning as above: report a closed breaker without linking the kb client. */
static const char *g_kb_breaker_state = "closed";

void kb_client_dependency_health(kb_client_dependency_health_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   snprintf(out->state, sizeof(out->state), "%s", g_kb_breaker_state);
}

int handle_kb_build(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.build");
}
int handle_kb_update(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.update");
}
int handle_kb_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.ingest");
}
int handle_kb_docs_push(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.docs.push");
}
int handle_kb_ingest_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.ingest.status");
}
int handle_kb_reembed(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.reembed");
}
int handle_memory_embed(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "memory.embed");
}
int handle_kb_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "kb.status");
}
int handle_optimize_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "optimize.export");
}
int handle_optimize_promote(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "optimize.promote");
}
int handle_optimize_replay_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "optimize.replay_record");
}
int handle_calibration_readiness(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "calibration.readiness");
}
int handle_demotion_check(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "demotion.check");
}
int handle_ranker_export_view(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "ranker.export_view");
}
int handle_ranker_fit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "ranker.fit");
}
int handle_workers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workers");
}
int handle_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "rules.list");
}
int handle_rules_generate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "rules.generate");
}
int handle_rules_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "rules.delete");
}
int handle_skill_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.list");
}
int handle_skill_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.show");
}
int handle_skill_lint(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.lint");
}
int handle_skill_eval(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.eval");
}
int handle_skill_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.create");
}
int handle_skill_edit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.edit");
}
int handle_skill_patch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.patch");
}
int handle_skill_archive(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.archive");
}
int handle_skill_pin(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jpinned = cJSON_GetObjectItemCaseSensitive(req, "pinned");
   return stub_handler(conn, cJSON_IsTrue(jpinned) ? "skill.pin" : "skill.unpin");
}
int handle_skill_lifecycle(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.lifecycle");
}
int handle_skill_autostub(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "skill.autostub");
}
int handle_collab_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "collab_rules.list");
}
int handle_collab_rules_list_active(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "collab_rules.list_active");
}
int handle_collab_rules_approve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "collab_rules.approve");
}
int handle_collab_rules_reject(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "collab_rules.reject");
}
int handle_collab_rules_retire(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "collab_rules.retire");
}
int handle_wm_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.set");
}
int handle_wm_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.get");
}
int handle_wm_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.list");
}
int handle_wm_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "wm.context");
}
int handle_primary_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "primary.set");
}
int handle_hosts_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "hosts.list");
}
int handle_embedders_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "embedders.list");
}
int handle_primary_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "primary.get");
}
int handle_primary_clear(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "primary.clear");
}
int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "attempt.record");
}
int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "attempt.list");
}
int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.metrics");
}
int handle_economizer_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "economizer.stats");
}
int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.delegations");
}
int handle_dashboard_traces(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.traces");
}
int handle_dashboard_plans(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.plans");
}
int handle_dashboard_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.logs");
}
int handle_dashboard_onboard(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.onboard");
}
int handle_dashboard_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.memory_stats");
}
int handle_lsp_diagnostics_summary(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "lsp.diagnostics_summary");
}
int handle_dashboard_all(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.all");
}
int handle_dashboard_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dashboard.audit");
}
char *server_agent_list_json(void)
{
   return strdup("[]");
}
int handle_workspace_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.context");
}
int handle_workspace_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.add");
}
int handle_workspace_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.list");
}
int handle_workspace_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.get");
}
int handle_workspace_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.remove");
}
int handle_workspace_mirror_sync(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "workspace.mirror-sync");
}
int handle_runner_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "runner.poll");
}
int handle_runner_respond(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "runner.respond");
}
int handle_dogfood_tag(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dogfood.tag");
}
int handle_dogfood_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dogfood.review");
}
int handle_dogfood_report(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dogfood.report");
}
int handle_identity_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "identity.show");
}
int handle_identity_snapshot(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "identity.snapshot");
}
int handle_identity_diff(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "identity.diff");
}
int handle_tool_execute(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "tool.execute");
}
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate");
}
int handle_delegate_aggregate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.aggregate");
}
int handle_delegate_reservation_forget(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.reservation.forget");
}
int handle_delegate_cancel_unassigned(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.cancel_unassigned");
}
int handle_roundtable_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "roundtable.review");
}
int handle_dev_sweep(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "dev.sweep");
}
int handle_delegate_launch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.launch");
}
int handle_delegate_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.status");
}
int handle_jobs_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "jobs.list");
}
int handle_jobs_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "jobs.status");
}
int handle_jobs_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "jobs.logs");
}
int handle_jobs_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "jobs.cancel");
}
int handle_coord_job_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "job.start");
}
int handle_coord_job_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "job.list");
}
int handle_coord_job_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "job.status");
}
int handle_coord_job_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "job.cancel");
}
int handle_aux_config_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "aux.config_show");
}
int handle_aux_test(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "aux.test");
}
int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.reply");
}
int handle_delegate_log(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "delegate.log");
}
int handle_episode_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "episode.list");
}
int handle_agent_episodes(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.episodes");
}
int handle_chat_send_stream(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "chat.send_stream");
}

int handle_agent_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.list");
}
int handle_agent_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.add");
}
int handle_agent_local(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.local");
}
int handle_agent_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.remove");
}
int handle_agent_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.enable");
}
int handle_agent_roles(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.roles");
}
int handle_agent_personas(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.personas");
}
int handle_agent_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.set");
}
int handle_agent_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.disable");
}
int handle_agent_probe(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.probe");
}
int handle_agent_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.stats");
}
int handle_agent_draft(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.draft");
}
int handle_agent_setup(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.setup");
}
int handle_agent_setup_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.setup_poll");
}
int handle_agent_cli_oauth_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.cli_oauth_start");
}
int handle_agent_cli_oauth_code(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.cli_oauth_code");
}
int handle_agent_cli_oauth_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "model.cli_oauth_poll");
}

int handle_mcp_tools_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "mcp.tools_list");
}

int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "mcp.call");
}

int handle_get_help(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "help.get");
}

int handle_mcp_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "mcp.audit");
}

int handle_mcp_recheck(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "mcp.recheck");
}

int handle_toolset_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "toolset.list");
}

int handle_toolset_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "toolset.show");
}

int handle_toolset_resolve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "toolset.resolve");
}

int handle_cron_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.list");
}
int handle_cron_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.add");
}
int handle_cron_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.show");
}
int handle_cron_history(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.history");
}
int handle_cron_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.run");
}
int handle_cron_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.enable");
}
int handle_cron_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.disable");
}
int handle_cron_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return stub_handler(conn, "cron.remove");
}

int is_safe_id(const char *s)
{
   (void)s;
   return 1;
}

char *shell_escape(const char *raw)
{
   return strdup(raw ? raw : "");
}

int config_load(config_t *cfg)
{
   if (g_config_stateful)
   {
      *cfg = g_config_snapshot;
      return 0;
   }
   memset(cfg, 0, sizeof(*cfg));
   return 0;
}

/* The config_load stub above always succeeds, so the probe the handlers now use
 * in its place has to say so too -- otherwise config.show / config.get would
 * report "failed to load config" against a config this suite considers loaded. */
int config_present(void)
{
   return 1;
}

/* api.disable persists through this now instead of load_file/save here. The
 * stateful mode's disk copy is what it would write, so mirror the port change
 * onto it; non-stateful is a no-op success, matching the config_save stub. */
int config_disable_api_http_listener(void)
{
   if (g_config_stateful)
      g_config_disk.server_api_http_port = 0;
   return 0;
}

int config_set_api_http_listener(int http_port, int rate_limit_per_min)
{
   if (http_port <= 0 || rate_limit_per_min <= 0)
      return -1;
   if (g_config_stateful)
   {
      g_config_disk.server_api_http_port = http_port;
      g_config_disk.server_api_rate_limit_per_min = rate_limit_per_min;
   }
   return 0;
}

/* The generated accessors read every field through this. Serve them out of the
 * SAME in-memory config the config_load stub returns, so an accessor and a
 * config_load observed in one test can never disagree. Non-stateful mode zeroes,
 * matching the config_load stub above. */
int config_field_read(size_t offset, size_t size, void *dst)
{
   if (!dst || size == 0)
      return -1;
   if (g_config_stateful)
      memcpy(dst, (const char *)&g_config_snapshot + offset, size);
   else
      memset(dst, 0, size);
   return 0;
}

int config_load_file(config_t *cfg)
{
   if (g_config_stateful)
   {
      *cfg = g_config_disk;
      return 0;
   }
   memset(cfg, 0, sizeof(*cfg));
   return 0;
}

/* live-config-reload P1b: server_config.c / server.c call config_reload after a config.set
 * and on SIGHUP; stub it here (this test doesn't link the real config.o). */
int config_reload(void)
{
   if (g_config_stateful)
   {
      g_config_snapshot = g_config_disk;
      g_config_reload_calls++;
   }
   return 0;
}

/* The provider endpoint writes through config_set now instead of mutating a
 * config_t and calling config_save. Mirror what the real one does against this
 * file's simulated state: patch the field on "disk" AND republish it to the
 * snapshot, so a following config_load observes the write. */
int config_set(const char *key, const char *value)
{
   const config_field_t *f = config_field_lookup(key);
   if (!f || !value)
      return -1;
   if (g_config_stateful)
   {
      if (config_field_set_value(&g_config_disk, f, value) != 0)
         return -1;
      (void)config_field_set_value(&g_config_snapshot, f, value);
   }
   return 0;
}

/* Enrolled-bearer writes go through the config module now instead of mutating
 * server_api_bearer_extra on a config_t. Mirror the config_save stub's model:
 * apply to the simulated disk AND the snapshot, so a read-back sees the write.
 * Note the config_save stub deliberately SCRUBS bearer state on the way to
 * disk (credentials live in Vault, not the config file), so these do the same. */
int config_server_api_bearer_extra_append(const char *token)
{
   if (!token || !token[0])
      return -1;
   if (!g_config_stateful)
      return 0;
   int slot = g_config_snapshot.server_api_bearer_extra_count;
   if (slot < 0 || slot >= AIMEE_API_BEARER_EXTRA_MAX)
      return -2;
   snprintf(g_config_snapshot.server_api_bearer_extra[slot],
            sizeof(g_config_snapshot.server_api_bearer_extra[0]), "%s", token);
   g_config_snapshot.server_api_bearer_extra_count = slot + 1;
   return slot;
}

int config_server_api_bearer_extra_clear(void)
{
   if (g_config_stateful)
   {
      memset(g_config_snapshot.server_api_bearer_extra, 0,
             sizeof(g_config_snapshot.server_api_bearer_extra));
      g_config_snapshot.server_api_bearer_extra_count = 0;
   }
   return 0;
}

int config_save(const config_t *cfg)
{
   if (g_config_stateful)
   {
      g_config_disk = *cfg;
      memset(g_config_disk.server_api_bearer_token, 0,
             sizeof(g_config_disk.server_api_bearer_token));
      memset(g_config_disk.server_api_bearer_extra, 0,
             sizeof(g_config_disk.server_api_bearer_extra));
      g_config_disk.server_api_bearer_extra_count = 0;
   }
   (void)cfg;
   return 0;
}

int config_secret_store(const char *name, const char *value)
{
   assert(name && strcmp(name, "AIMEE_KB_API_BEARER_TOKEN") == 0);
   g_config_secret_store_calls++;
   g_config_secret_store_configured = value && value[0] ? 1 : 0;
   return 0;
}

/* config_fields.o resolves the economizer mode through these pure helpers; this
 * test does not link the real config.o. */
const char *econ_mode_name(int mode)
{
   return mode == ECON_MODE_AGGRESSIVE ? "aggressive" : mode == ECON_MODE_SAFE ? "safe" : "off";
}

int econ_mode_parse(const char *s)
{
   if (s && strcmp(s, "off") == 0)
      return ECON_MODE_OFF;
   if (s && strcmp(s, "safe") == 0)
      return ECON_MODE_SAFE;
   if (s && strcmp(s, "aggressive") == 0)
      return ECON_MODE_AGGRESSIVE;
   return -1;
}

const char *config_output_dir(void)
{
   return "/tmp";
}

const char *config_guardrail_mode(void)
{
   return "off";
}

void session_state_load(session_state_t *state, const char *sid)
{
   (void)sid;
   memset(state, 0, sizeof(*state));
}

void session_state_save(const session_state_t *state, const char *sid)
{
   (void)sid;
   if (state)
   {
      g_saved_state = *state;
      g_session_state_save_calls++;
   }
}

int worktree_create_sibling(const char *git_root, const char *session_id, const char *work_name)
{
   (void)git_root;
   (void)session_id;
   (void)work_name;
   return 0;
}

int worktree_sibling_path(const char *git_root, const char *session_id, const char *work_name,
                          char *out, size_t out_len)
{
   (void)work_name;
   snprintf(out, out_len, "%s/.aimee/worktrees/%.8s/main", git_root, session_id);
   return 0;
}

const char *worktree_for_cwd(const session_state_t *state, const char *cwd)
{
   if (!state || !cwd)
      return NULL;
   for (int i = 0; i < state->worktree_count; i++)
   {
      size_t len = strlen(state->worktrees[i].git_root);
      if (strncmp(cwd, state->worktrees[i].git_root, len) == 0 &&
          (cwd[len] == '/' || cwd[len] == '\0'))
         return state->worktrees[i].worktree_path;
   }
   return NULL;
}

int pre_tool_check(const char *tool_name, const char *tool_input, session_state_t *state,
                   const char *guardrail_mode, const char *cwd, char *msg, size_t msg_len)
{
   (void)tool_name;
   (void)tool_input;
   (void)guardrail_mode;
   (void)cwd;
   if (state)
   {
      g_pre_tool_state = *state;
      g_pre_tool_seen_state = 1;
   }
   if (msg_len > 0)
      msg[0] = '\0';
   return 0;
}

void post_tool_update(const char *tool_name, const char *tool_input, session_state_t *state)
{
   (void)tool_name;
   (void)tool_input;
   (void)state;
}

static void test_invalid_json(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *json = dispatch_json(ctx, conn, "{", 1);
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "error") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "invalid JSON") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_missing_method(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *json =
       dispatch_json(ctx, conn, "{\"request_id\":\"r1\"}", strlen("{\"request_id\":\"r1\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "missing method") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_oversized_payload(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   size_t size = LIMIT_MEMORY + 64;
   char *msg = malloc(size + 1);
   assert(msg != NULL);
   snprintf(msg, size + 1, "{\"method\":\"memory.list\",\"pad\":\"");
   size_t used = strlen(msg);
   memset(msg + used, 'a', size - used - 2);
   msg[size - 2] = '"';
   msg[size - 1] = '}';
   msg[size] = '\0';
   cJSON *json = dispatch_json(ctx, conn, msg, size);
   assert(strstr(cJSON_GetObjectItem(json, "message")->valuestring, "PAYLOAD_TOO_LARGE") != NULL);
   cJSON_Delete(json);
   free(msg);
   free(conn);
   free(ctx);
}

static void test_large_delegate_payload_within_limit(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->peer_uid = getuid();
   conn->capabilities = CAPS_AUTHENTICATED;

   size_t size = (2 * 1024 * 1024) + 256;
   char *msg = malloc(size + 1);
   assert(msg != NULL);
   snprintf(msg, size + 1, "{\"method\":\"delegate\",\"prompt\":\"");
   size_t used = strlen(msg);
   memset(msg + used, 'a', size - used - 2);
   msg[size - 2] = '"';
   msg[size - 1] = '}';
   msg[size] = '\0';

   cJSON *json = dispatch_json(ctx, conn, msg, size);
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "delegate") == 0);
   cJSON_Delete(json);
   free(msg);
   free(conn);
   free(ctx);
}

static void test_large_mcp_call_payload_within_limit(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->peer_uid = getuid();
   conn->capabilities = CAPS_AUTHENTICATED;

   size_t size = LIMIT_DEFAULT + 64;
   char *msg = malloc(size + 1);
   assert(msg != NULL);
   snprintf(msg, size + 1,
            "{\"method\":\"mcp.call\",\"tool\":\"roundtable_review\",\"arguments\":{\"diff\":\"");
   size_t used = strlen(msg);
   memset(msg + used, 'a', size - used - 4);
   msg[size - 4] = '"';
   msg[size - 3] = '}';
   msg[size - 2] = '}';
   msg[size - 1] = '\0';

   cJSON *json = dispatch_json(ctx, conn, msg, strlen(msg));
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "mcp.call") == 0);
   cJSON_Delete(json);
   free(msg);
   free(conn);
   free(ctx);
}

static void test_large_roundtable_payload_within_limit(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->peer_uid = getuid();
   conn->capabilities = CAPS_AUTHENTICATED;

   size_t size = LIMIT_DEFAULT + 64;
   char *msg = malloc(size + 1);
   assert(msg != NULL);
   snprintf(msg, size + 1, "{\"method\":\"roundtable.review\",\"artifact\":\"");
   size_t used = strlen(msg);
   memset(msg + used, 'a', size - used - 2);
   msg[size - 2] = '"';
   msg[size - 1] = '}';
   msg[size] = '\0';

   cJSON *json = dispatch_json(ctx, conn, msg, size);
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "roundtable.review") == 0);
   cJSON_Delete(json);
   free(msg);
   free(conn);
   free(ctx);
}

static void test_unknown_method(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"unknown.method\"}",
                               strlen("{\"method\":\"unknown.method\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "unknown method") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "code")->valuestring, "UNKNOWN_METHOD") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "method")->valuestring, "unknown.method") == 0);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_removed_storage_named_migration_alias(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAPS_AUTHENTICATED;

   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"migrate.db2_to_postgres\"}",
                               strlen("{\"method\":\"migrate.db2_to_postgres\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "unknown method") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "code")->valuestring, "UNKNOWN_METHOD") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"migrate.v2\",\"source_path\":\"/tmp/x\"}",
                        strlen("{\"method\":\"migrate.v2\",\"source_path\":\"/tmp/x\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "message")->valuestring, "unknown method") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "code")->valuestring, "UNKNOWN_METHOD") == 0);
   cJSON_Delete(json);

   free(conn);
   free(ctx);
}

static void test_authz_denied_shape(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->peer_uid = 4242;
   conn->capabilities = 0;
   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"tool.execute\"}",
                               strlen("{\"method\":\"tool.execute\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "code")->valuestring, "AUTHZ_DENIED") == 0);
   assert(strcmp(cJSON_GetObjectItem(json, "method")->valuestring, "tool.execute") == 0);
   assert(strstr(cJSON_GetObjectItem(json, "principal")->valuestring, "uid:4242") != NULL);
   assert(cJSON_GetObjectItem(json, "required_caps") != NULL);
   assert(cJSON_GetObjectItem(json, "held_caps") != NULL);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
}

static void test_routing(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAPS_AUTHENTICATED;

   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"server.info\"}",
                               strlen("{\"method\":\"server.info\"}"));
   assert(cJSON_GetObjectItem(json, "protocol_version") != NULL);
   cJSON *methods = cJSON_GetObjectItem(json, "methods");
   assert(cJSON_IsArray(methods));
   int has_delegate_status = 0;
   int has_delegate_launch = 0;
   int has_launch_run = 0;
   int has_jobs_logs = 0;
   int has_coord_job_status = 0;
   int has_dogfood_review = 0;
   int has_eval_run = 0;
   int has_cron_run = 0;
   int has_cron_add = 0;
   int has_trajectory_export = 0;
   int has_trajectory_batch = 0;
   cJSON *m;
   cJSON_ArrayForEach(m, methods)
   {
      if (cJSON_IsString(m) && strcmp(m->valuestring, "delegate.status") == 0)
         has_delegate_status = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "delegate.launch") == 0)
         has_delegate_launch = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "launch.run") == 0)
         has_launch_run = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "jobs.logs") == 0)
         has_jobs_logs = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "job.status") == 0)
         has_coord_job_status = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "dogfood.review") == 0)
         has_dogfood_review = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "eval.run") == 0)
         has_eval_run = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "cron.run") == 0)
         has_cron_run = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "cron.add") == 0)
         has_cron_add = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "trajectory.export") == 0)
         has_trajectory_export = 1;
      if (cJSON_IsString(m) && strcmp(m->valuestring, "trajectory.batch") == 0)
         has_trajectory_batch = 1;
   }
   assert(has_delegate_status);
   assert(has_delegate_launch);
   assert(has_launch_run);
   assert(has_jobs_logs);
   assert(has_coord_job_status);
   assert(has_dogfood_review);
   assert(has_eval_run);
   assert(has_cron_run);
   assert(has_cron_add);
   assert(has_trajectory_export);
   assert(has_trajectory_batch);
   cJSON_Delete(json);

   json =
       dispatch_json(ctx, conn, "{\"method\":\"help.get\"}", strlen("{\"method\":\"help.get\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "help.get") == 0);
   assert(strcmp(g_last_handler, "help.get") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"session.list\"}",
                        strlen("{\"method\":\"session.list\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "session.list") == 0);
   assert(strcmp(g_last_handler, "session.list") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"session.brief\"}",
                        strlen("{\"method\":\"session.brief\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "session.brief") == 0);
   assert(strcmp(g_last_handler, "session.brief") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"trajectory.export\",\"session_id\":\"s1\"}",
                        strlen("{\"method\":\"trajectory.export\",\"session_id\":\"s1\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "trajectory.export") == 0);
   assert(strcmp(g_last_handler, "trajectory.export") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"trajectory.batch\",\"tasks_path\":\"tasks\"}",
                        strlen("{\"method\":\"trajectory.batch\",\"tasks_path\":\"tasks\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "trajectory.batch") == 0);
   assert(strcmp(g_last_handler, "trajectory.batch") == 0);
   cJSON_Delete(json);

   /* Auditable-correctness P1: /v1/audit/trace's op must resolve to the
    * server-side KB-forward handler (regression guard for the bug where the
    * cloned KB-proxy route never reached the KB). */
   json = dispatch_json(
       ctx, conn, "{\"method\":\"evidence.trace_retrieval_event\",\"turn_id\":\"t1\"}",
       strlen("{\"method\":\"evidence.trace_retrieval_event\",\"turn_id\":\"t1\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring,
                 "evidence.trace_retrieval_event") == 0);
   assert(strcmp(g_last_handler, "evidence.trace_retrieval_event") == 0);
   cJSON_Delete(json);

   /* Auditable-correctness P2: /v1/audit/provenance's op must likewise resolve to
    * its server-side KB-forward handler. */
   json = dispatch_json(
       ctx, conn, "{\"method\":\"evidence.provenance_retrieval_event\",\"turn_id\":\"t1\"}",
       strlen("{\"method\":\"evidence.provenance_retrieval_event\",\"turn_id\":\"t1\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring,
                 "evidence.provenance_retrieval_event") == 0);
   assert(strcmp(g_last_handler, "evidence.provenance_retrieval_event") == 0);
   cJSON_Delete(json);

   /* Auditable-correctness P3: /v1/audit/fidelity's op must likewise resolve to
    * its server-side KB-forward handler. */
   json = dispatch_json(
       ctx, conn, "{\"method\":\"evidence.fidelity_retrieval_event\",\"turn_id\":\"t1\"}",
       strlen("{\"method\":\"evidence.fidelity_retrieval_event\",\"turn_id\":\"t1\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring,
                 "evidence.fidelity_retrieval_event") == 0);
   assert(strcmp(g_last_handler, "evidence.fidelity_retrieval_event") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"rules.delete\",\"id\":1}",
                        strlen("{\"method\":\"rules.delete\",\"id\":1}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "rules.delete") == 0);
   assert(strcmp(g_last_handler, "rules.delete") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"skill.list\"}",
                        strlen("{\"method\":\"skill.list\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "skill.list") == 0);
   assert(strcmp(g_last_handler, "skill.list") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"skill.show\",\"name\":\"review\"}",
                        strlen("{\"method\":\"skill.show\",\"name\":\"review\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "skill.show") == 0);
   assert(strcmp(g_last_handler, "skill.show") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn,
                        "{\"method\":\"skill.patch\",\"name\":\"review\","
                        "\"old_string\":\"a\",\"new_string\":\"b\"}",
                        strlen("{\"method\":\"skill.patch\",\"name\":\"review\","
                               "\"old_string\":\"a\",\"new_string\":\"b\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "skill.patch") == 0);
   assert(strcmp(g_last_handler, "skill.patch") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"skill.unpin\",\"name\":\"review\"}",
                        strlen("{\"method\":\"skill.unpin\",\"name\":\"review\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "skill.unpin") == 0);
   assert(strcmp(g_last_handler, "skill.unpin") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"skill.lifecycle\"}",
                        strlen("{\"method\":\"skill.lifecycle\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "skill.lifecycle") == 0);
   assert(strcmp(g_last_handler, "skill.lifecycle") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"skill.autostub\"}",
                        strlen("{\"method\":\"skill.autostub\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "skill.autostub") == 0);
   assert(strcmp(g_last_handler, "skill.autostub") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"delegate.status\",\"job_id\":1}",
                        strlen("{\"method\":\"delegate.status\",\"job_id\":1}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "delegate.status") == 0);
   assert(strcmp(g_last_handler, "delegate.status") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"roundtable.review\",\"prompt\":\"draft\"}",
                        strlen("{\"method\":\"roundtable.review\",\"prompt\":\"draft\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "roundtable.review") == 0);
   assert(strcmp(g_last_handler, "roundtable.review") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"delegate.launch\"}",
                        strlen("{\"method\":\"delegate.launch\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "delegate.launch") == 0);
   assert(strcmp(g_last_handler, "delegate.launch") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"jobs.logs\",\"job_id\":1}",
                        strlen("{\"method\":\"jobs.logs\",\"job_id\":1}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "jobs.logs") == 0);
   assert(strcmp(g_last_handler, "jobs.logs") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"job.status\",\"job_id\":1}",
                        strlen("{\"method\":\"job.status\",\"job_id\":1}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "job.status") == 0);
   assert(strcmp(g_last_handler, "job.status") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"eval.run\",\"suite_dir\":\"evals/delegate\"}",
                        strlen("{\"method\":\"eval.run\",\"suite_dir\":\"evals/delegate\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   cJSON *run_results = cJSON_GetObjectItem(json, "results");
   assert(cJSON_IsArray(run_results));
   cJSON *run_row = cJSON_GetArrayItem(run_results, 0);
   assert(cJSON_GetObjectItem(run_row, "rescue_recoveries")->valueint == 2);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"eval.results\"}",
                        strlen("{\"method\":\"eval.results\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   cJSON *rows = cJSON_GetObjectItem(json, "results");
   assert(cJSON_IsArray(rows));
   cJSON *row = cJSON_GetArrayItem(rows, 0);
   assert(cJSON_GetObjectItem(row, "rescue_recoveries")->valueint == 2);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn, "{\"method\":\"cron.run\",\"job_id\":\"pulse\"}",
                        strlen("{\"method\":\"cron.run\",\"job_id\":\"pulse\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "cron.run") == 0);
   assert(strcmp(g_last_handler, "cron.run") == 0);
   cJSON_Delete(json);

   json = dispatch_json(ctx, conn,
                        "{\"method\":\"cron.add\",\"job_id\":\"pulse\",\"schedule\":\"every "
                        "10m\",\"mode\":\"script\",\"script\":\"echo OK\"}",
                        strlen("{\"method\":\"cron.add\",\"job_id\":\"pulse\",\"schedule\":\"every "
                               "10m\",\"mode\":\"script\",\"script\":\"echo OK\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "route")->valuestring, "cron.add") == 0);
   assert(strcmp(g_last_handler, "cron.add") == 0);
   cJSON_Delete(json);

   free(conn);
   free(ctx);
}

static void test_init_route_through_server_to_kb(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAPS_AUTHENTICATED;

   g_last_exec_cmd[0] = '\0';
   g_exec_timeout_ms = 0;
   cJSON *json =
       dispatch_json(ctx, conn, "{\"method\":\"init.run\"}", strlen("{\"method\":\"init.run\"}"));
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   assert(strstr(g_last_exec_cmd, "aimee-kb") != NULL);
   assert(strstr(g_last_exec_cmd, "--bootstrap-db2 --json") != NULL);
   assert(g_exec_timeout_ms == 300000);
   cJSON_Delete(json);

   free(conn);
   free(ctx);
}

static void test_launch_run_returns_provider_metadata(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAPS_AUTHENTICATED;

   /* No-arg `aimee` launch issues launch.run. With config_load stubbed to
    * zero-init, the response should still carry a session_id and the
    * default "claude" provider rather than the unported-command error. */
   snprintf(g_git_repo_root_prefix, sizeof(g_git_repo_root_prefix), "%s", "/tmp/proj");
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", "/tmp/proj");
   memset(&g_saved_state, 0, sizeof(g_saved_state));
   g_session_state_save_calls = 0;

   cJSON *json = dispatch_json(ctx, conn, "{\"method\":\"launch.run\",\"cwd\":\"/tmp/proj/src\"}",
                               strlen("{\"method\":\"launch.run\",\"cwd\":\"/tmp/proj/src\"}"));
   cJSON *status = cJSON_GetObjectItem(json, "status");
   assert(status != NULL && cJSON_IsString(status));
   assert(strcmp(status->valuestring, "ok") == 0);
   cJSON *provider = cJSON_GetObjectItem(json, "provider");
   assert(provider != NULL && cJSON_IsString(provider));
   assert(strcmp(provider->valuestring, "claude") == 0);
   cJSON *sid = cJSON_GetObjectItem(json, "session_id");
   assert(sid != NULL && cJSON_IsString(sid));
   assert(strlen(sid->valuestring) > 0);
   cJSON *builtin = cJSON_GetObjectItem(json, "builtin");
   assert(builtin != NULL);
   assert(cJSON_IsBool(builtin) && cJSON_IsTrue(builtin));
   cJSON *worktree_cwd = cJSON_GetObjectItem(json, "worktree_cwd");
   assert(worktree_cwd != NULL && cJSON_IsString(worktree_cwd));
   assert(strstr(worktree_cwd->valuestring, "/tmp/proj/.aimee/worktrees/") != NULL);
   assert(strstr(worktree_cwd->valuestring, "/main/src") != NULL);
   assert(g_session_state_save_calls > 0);
   assert(g_saved_state.worktree_count == 1);
   cJSON_Delete(json);

   g_git_repo_root_prefix[0] = '\0';
   g_git_repo_root_value[0] = '\0';
   free(conn);
   free(ctx);
}

static void test_hooks_pre_recovers_worktree_mapping_from_cwd(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);

   snprintf(g_git_repo_root_prefix, sizeof(g_git_repo_root_prefix), "%s", "/tmp/project");
   snprintf(g_git_repo_root_value, sizeof(g_git_repo_root_value), "%s", "/tmp/project");
   memset(&g_saved_state, 0, sizeof(g_saved_state));
   memset(&g_pre_tool_state, 0, sizeof(g_pre_tool_state));
   g_session_state_save_calls = 0;
   g_pre_tool_seen_state = 0;

   const char *req = "{\"method\":\"hooks.pre\",\"session_id\":\"sessabcdef\","
                     "\"tool_name\":\"Read\",\"tool_input\":{},\"cwd\":\"/tmp/project/src\"}";
   cJSON *json = dispatch_json(ctx, conn, req, strlen(req));
   assert(strcmp(cJSON_GetObjectItem(json, "status")->valuestring, "ok") == 0);
   assert(g_pre_tool_seen_state == 1);
   assert(g_pre_tool_state.worktree_count == 1);
   assert(strcmp(g_pre_tool_state.worktrees[0].git_root, "/tmp/project") == 0);
   assert(strcmp(g_pre_tool_state.worktrees[0].worktree_path,
                 "/tmp/project/.aimee/worktrees/sessabcd/main") == 0);
   assert(g_session_state_save_calls == 1);
   assert(g_saved_state.worktree_count == 1);
   cJSON_Delete(json);

   g_git_repo_root_prefix[0] = '\0';
   g_git_repo_root_value[0] = '\0';
   free(conn);
   free(ctx);
}

/* Regression: the /v1 HTTP workers (server_http.c) build a memset-zeroed
 * server_conn_t, so conn->evloop is NULL and its fd is never registered with
 * any epoll set. Flushing a buffered response must NOT route that NULL loop
 * into the event layer — conn_update_events used to call
 * platform_evloop_mod(conn->evloop, ...) unconditionally, so epoll_ctl
 * dereferenced NULL and segfaulted the whole server on every buffered /v1
 * response (a crash-restart loop). conn_update_events now no-ops on NULL. */
static void test_conn_update_events_null_evloop(void)
{
   int fds[2];
   assert(pipe(fds) == 0);

   /* Case 1: NULL evloop (the HTTP-worker shape). Preload pending bytes so
    * server_send_response drains them via conn_flush, which is the path that
    * calls conn_update_events. The event layer must never see the NULL loop. */
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(conn != NULL);
   conn->fd = fds[1];
   conn->evloop = NULL;
   memcpy(conn->write_buf, "hello", 5);
   conn->write_len = 5;
   conn->write_pos = 0;

   g_evloop_mod_null_loop = 0;
   g_evloop_mod_nonnull_loop = 0;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   assert(rc == 0);                     /* response actually written */
   assert(g_evloop_mod_null_loop == 0); /* the fix: NULL loop never used */
   free(conn);

   /* Case 2 (positive control): an epoll-registered conn (non-NULL evloop)
    * must still have its interest set updated — the fix must not disable that
    * for real event-loop connections. */
   char marker;
   conn = calloc(1, sizeof(*conn));
   assert(conn != NULL);
   conn->fd = fds[1];
   conn->evloop = (platform_evloop_t *)&marker; /* non-NULL; stub never derefs */
   memcpy(conn->write_buf, "world", 5);
   conn->write_len = 5;
   conn->write_pos = 0;

   g_evloop_mod_null_loop = 0;
   g_evloop_mod_nonnull_loop = 0;
   resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   assert(rc == 0);
   assert(g_evloop_mod_null_loop == 0);
   assert(g_evloop_mod_nonnull_loop == 1); /* registration still happens */
   free(conn);

   close(fds[0]);
   close(fds[1]);
   printf("test_conn_update_events_null_evloop: PASS\n");
}

/* Generic config RPCs may report whether a Vault record is configured, but
 * must never return the credential or route its mutation through aimee.yaml. */
static void test_config_secret_redaction_and_vault_write(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAP_SESSION_ADMIN;

   memset(&g_config_snapshot, 0, sizeof(g_config_snapshot));
   memset(&g_config_disk, 0, sizeof(g_config_disk));
   snprintf(g_config_snapshot.kb_api_bearer_token, sizeof(g_config_snapshot.kb_api_bearer_token),
            "%s", "never-echo-config-secret");
   g_config_stateful = 1;
   g_config_reload_calls = 0;
   g_config_secret_store_calls = 0;
   g_config_secret_store_configured = 0;

   const char *get_req = "{\"method\":\"config.get\",\"key\":\"kb_api_bearer_token\"}";
   cJSON *got = dispatch_json(ctx, conn, get_req, strlen(get_req));
   cJSON *value = cJSON_GetObjectItemCaseSensitive(got, "value");
   cJSON *secret = cJSON_GetObjectItemCaseSensitive(got, "secret");
   assert(cJSON_IsBool(value) && cJSON_IsTrue(value));
   assert(cJSON_IsBool(secret) && cJSON_IsTrue(secret));
   char *serialized = cJSON_PrintUnformatted(got);
   assert(serialized && strstr(serialized, "never-echo-config-secret") == NULL);
   free(serialized);
   cJSON_Delete(got);

   const char *set_req = "{\"method\":\"config.set\",\"key\":\"kb_api_bearer_token\","
                         "\"value\":\"never-echo-new-secret\"}";
   cJSON *set = dispatch_json(ctx, conn, set_req, strlen(set_req));
   assert(g_config_secret_store_calls == 1 && g_config_secret_store_configured == 1);
   value = cJSON_GetObjectItemCaseSensitive(set, "value");
   secret = cJSON_GetObjectItemCaseSensitive(set, "secret");
   assert(cJSON_IsBool(value) && cJSON_IsTrue(value));
   assert(cJSON_IsBool(secret) && cJSON_IsTrue(secret));
   serialized = cJSON_PrintUnformatted(set);
   assert(serialized && strstr(serialized, "never-echo-new-secret") == NULL);
   free(serialized);
   cJSON_Delete(set);
   assert(g_config_reload_calls == 0);
   assert(g_config_disk.kb_api_bearer_token[0] == '\0');

   g_config_stateful = 0;
   free(conn);
   free(ctx);
   printf("test_config_secret_redaction_and_vault_write: PASS\n");
}

/* Regression: the server's config_load() is an immutable live snapshot. API
 * credential mutations must instead start from the latest disk image and
 * republish it, or a second sequential enrollment starts from zero extras and
 * silently replaces the first one. */
static void test_api_enroll_preserves_sequential_bearers(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   conn->capabilities = CAP_SESSION_ADMIN;

   memset(&g_config_disk, 0, sizeof(g_config_disk));
   assert(vault_runtime_secret_set("AIMEE_API_BEARER_TOKEN", "primary-test-bearer") == 0);
   g_config_snapshot = g_config_disk;
   g_config_reload_calls = 0;
   g_config_stateful = 1;
   server_http_set_bearer_extra(NULL, 0);

   const char *request = "{\"method\":\"api.enroll_bearer\"}";
   cJSON *first = dispatch_json(ctx, conn, request, strlen(request));
   assert(strcmp(cJSON_GetObjectItem(first, "status")->valuestring, "ok") == 0);
   assert(cJSON_GetObjectItem(first, "enrolled_count")->valueint == 1);
   char first_bearer[256];
   snprintf(first_bearer, sizeof(first_bearer), "%s",
            cJSON_GetObjectItem(first, "bearer_token")->valuestring);
   cJSON_Delete(first);

   cJSON *second = dispatch_json(ctx, conn, request, strlen(request));
   assert(strcmp(cJSON_GetObjectItem(second, "status")->valuestring, "ok") == 0);
   assert(cJSON_GetObjectItem(second, "enrolled_count")->valueint == 2);
   char second_bearer[256];
   snprintf(second_bearer, sizeof(second_bearer), "%s",
            cJSON_GetObjectItem(second, "bearer_token")->valuestring);
   cJSON_Delete(second);

   assert(strcmp(first_bearer, second_bearer) != 0);
   /* Enrolling touches VAULT only. config_save never persisted the enrolled set
    * (config_load migrates any legacy value out and scrubs the fields), so the
    * save+reload this used to do republished an unchanged config. No reload now. */
   assert(g_config_reload_calls == 0);
   assert(g_config_disk.server_api_bearer_token[0] == '\0');
   assert(g_config_disk.server_api_bearer_extra_count == 0);
   assert(g_config_snapshot.server_api_bearer_extra_count == 0);
   char stored[256];
   assert(runtime_secret_get("AIMEE_API_BEARER_TOKEN_EXTRA_0", stored, sizeof(stored)) == 1);
   assert(strcmp(stored, first_bearer) == 0);
   assert(runtime_secret_get("AIMEE_API_BEARER_TOKEN_EXTRA_1", stored, sizeof(stored)) == 1);
   assert(strcmp(stored, second_bearer) == 0);
   assert(server_http_enrolled_bearer_count() == 2);

   server_http_set_bearer_extra(NULL, 0);
   assert(vault_runtime_secret_delete("AIMEE_API_BEARER_TOKEN") == 0);
   assert(vault_runtime_secret_delete("AIMEE_API_BEARER_TOKEN_EXTRA_0") == 0);
   assert(vault_runtime_secret_delete("AIMEE_API_BEARER_TOKEN_EXTRA_1") == 0);
   g_config_stateful = 0;
   free(conn);
   free(ctx);
   printf("test_api_enroll_preserves_sequential_bearers: PASS\n");
}

/* session.brief_assemble (Proposal 1 Phase 1): the remote thin-client
 * SessionStart brief op. Asserts the response is the minimal versioned envelope
 * {schema_version:1, output} and that the assembled brief (here the stub's
 * marker) flows into output — the contract the thin client depends on. */
static void test_session_brief_assemble(void)
{
   server_ctx_t *ctx = calloc(1, sizeof(*ctx));
   server_conn_t *conn = calloc(1, sizeof(*conn));
   assert(ctx != NULL && conn != NULL);
   g_memory_scope_begin_calls = 0;
   g_memory_scope_clear_calls = 0;
   const char *msg = "{\"method\":\"session.brief_assemble\"}";
   cJSON *json = dispatch_json(ctx, conn, msg, strlen(msg));
   cJSON *sv = cJSON_GetObjectItem(json, "schema_version");
   cJSON *out = cJSON_GetObjectItem(json, "output");
   assert(cJSON_IsNumber(sv) && sv->valueint == 1);
   assert(cJSON_IsString(out) && strstr(out->valuestring, "STUB_BRIEF_CONTENT") != NULL);
   assert(g_memory_scope_begin_calls == 1);
   assert(g_memory_scope_clear_calls == 1);
   cJSON_Delete(json);
   free(conn);
   free(ctx);
   printf("test_session_brief_assemble: PASS\n");
}

/* The kb block in server.health is where "accepted, reports healthy, cannot work"
 * became visible to users: `status` was `reachable ? "ok" : "unreachable"`, and
 * every capability the kb reported sat beside it as a sibling that nothing read.
 * `aimee status` printed "aimee-kb: ok" one line above "embedder: not
 * configured" and both were true.
 *
 * These pin the three states apart, because the failure mode is not that any one
 * of them is wrong — it is that two of them used to collapse into one. */
static const char *kb_status_of(cJSON *resp)
{
   cJSON *kb = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   cJSON *s = kb ? cJSON_GetObjectItemCaseSensitive(kb, "status") : NULL;
   return cJSON_IsString(s) ? s->valuestring : NULL;
}

static void test_health_kb_verdict_states(void)
{
   /* 1. Nothing answered — the only case that may say unreachable. */
   kb_health_stub_reset();
   cJSON *resp = cJSON_CreateObject();
   server_health_add_kb(resp);
   assert(strcmp(kb_status_of(resp), "unreachable") == 0);
   cJSON_Delete(resp);

   /* 2. Answered and capable. */
   kb_health_stub_reset();
   g_kb_health_rc = 0;
   g_kb_health.process_ok = 1;
   snprintf(g_kb_health.status, sizeof(g_kb_health.status), "ok");
   resp = cJSON_CreateObject();
   server_health_add_kb(resp);
   assert(strcmp(kb_status_of(resp), "ok") == 0);
   cJSON_Delete(resp);

   /* 3. Answered and told us it cannot work. This is the case that used to
    * report "ok"; it must be degraded, NOT unreachable — the kb is up, and
    * sending an operator to debug the network would be a fresh wrong answer. */
   kb_health_stub_reset();
   g_kb_health_rc = 0;
   g_kb_health.process_ok = 1;
   snprintf(g_kb_health.status, sizeof(g_kb_health.status), "degraded");
   snprintf(g_kb_health.blockers, sizeof(g_kb_health.blockers),
            "no embedder configured: set embedder_model\nvector table missing");
   resp = cJSON_CreateObject();
   server_health_add_kb(resp);
   assert(strcmp(kb_status_of(resp), "degraded") == 0);
   /* The reasons reach the client, split back into one string per blocker. */
   cJSON *kb = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   cJSON *blockers = cJSON_GetObjectItemCaseSensitive(kb, "blockers");
   assert(cJSON_IsArray(blockers) && cJSON_GetArraySize(blockers) == 2);
   assert(strcmp(cJSON_GetArrayItem(blockers, 0)->valuestring,
                 "no embedder configured: set embedder_model") == 0);
   assert(strcmp(cJSON_GetArrayItem(blockers, 1)->valuestring, "vector table missing") == 0);
   cJSON_Delete(resp);

   /* Warnings travel even though they do not move the verdict. This block used to
    * drop the kb's warnings array entirely, so an advisory finding reached no
    * operator: a typed-fact backlog nothing could drain sat unreported for hours
    * behind a status of "ok". Publishing a finding into a field no surface renders
    * is the same defect as never computing it. */
   kb_health_stub_reset();
   g_kb_health_rc = 0;
   g_kb_health.process_ok = 1;
   snprintf(g_kb_health.status, sizeof(g_kb_health.status), "ok");
   snprintf(g_kb_health.warnings, sizeof(g_kb_health.warnings),
            "typed-fact extraction: 4 job(s) queued with nothing to drain them\nKB not ingested "
            "in over 7 days");
   resp = cJSON_CreateObject();
   server_health_add_kb(resp);
   assert(strcmp(kb_status_of(resp), "ok") == 0); /* advisory: verdict unchanged */
   kb = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   cJSON *warns = cJSON_GetObjectItemCaseSensitive(kb, "warnings");
   assert(cJSON_IsArray(warns) && cJSON_GetArraySize(warns) == 2);
   assert(strstr(cJSON_GetArrayItem(warns, 0)->valuestring, "typed-fact extraction") != NULL);
   /* No blockers key at all when there are none, rather than an empty array. */
   assert(cJSON_GetObjectItemCaseSensitive(kb, "blockers") == NULL);
   cJSON_Delete(resp);

   /* 4. An open transport breaker refuses every call locally, so a kb that
    * considers itself perfectly healthy still cannot be queried. The breaker is
    * part of the verdict rather than a flag beside it. */
   kb_health_stub_reset();
   g_kb_health_rc = 0;
   g_kb_health.process_ok = 1;
   snprintf(g_kb_health.status, sizeof(g_kb_health.status), "ok");
   g_kb_breaker_state = "open";
   resp = cJSON_CreateObject();
   server_health_add_kb(resp);
   assert(strcmp(kb_status_of(resp), "degraded") == 0);
   kb = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(kb, "queries_suppressed")));
   cJSON_Delete(resp);
   g_kb_breaker_state = "closed";

   /* 5. An older kb sends no verdict. Absence is not a blocker, and must not be
    * read as one — inventing "degraded" out of silence would make every
    * pre-upgrade install look broken. */
   kb_health_stub_reset();
   g_kb_health_rc = 0;
   g_kb_health.process_ok = 1;
   resp = cJSON_CreateObject();
   server_health_add_kb(resp);
   assert(strcmp(kb_status_of(resp), "ok") == 0);
   cJSON_Delete(resp);

   kb_health_stub_reset();
}

int main(void)
{
   test_health_kb_verdict_states();
   test_invalid_json();
   test_session_brief_assemble();
   test_conn_update_events_null_evloop();
   test_config_secret_redaction_and_vault_write();
   test_api_enroll_preserves_sequential_bearers();
   test_missing_method();
   test_oversized_payload();
   test_large_delegate_payload_within_limit();
   test_large_roundtable_payload_within_limit();
   test_large_mcp_call_payload_within_limit();
   test_unknown_method();
   test_removed_storage_named_migration_alias();
   test_authz_denied_shape();
   test_routing();
   test_init_route_through_server_to_kb();
   test_launch_run_returns_provider_metadata();
   test_hooks_pre_recovers_worktree_mapping_from_cwd();
   printf("server_dispatch: all tests passed\n");
   return 0;
}

const char *config_embedder_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   if (cfg && cfg->embedder_command[0])
      return cfg->embedder_command;
   return MEMORY_EMBED_TEST_FIXTURE;
}
