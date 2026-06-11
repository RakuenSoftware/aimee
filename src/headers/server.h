#ifndef DEC_SERVER_H
#define DEC_SERVER_H 1

#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include "compute_pool.h"
#include "platform_event.h"
#include "provider_catalog.h"

/* Forward declaration */
typedef struct cJSON cJSON;

#define SERVER_PROTOCOL_VERSION 1
/* Per-server cap on concurrent connections held in the dispatch table.
 * Paired with SERVER_LISTEN_BACKLOG: the kernel queue holds connects
 * until accept() pulls them, then this cap bounds how many fds the
 * server tracks simultaneously. When at the cap, accept_connection
 * close()s the fd silently — which clients see as EOF mid-handshake,
 * surfaced as a misleading "token mismatch?" since auth was the first
 * RPC after connect. 64 was too low once long-lived MCP/webchat
 * connections plus bursty PreToolUse hooks across multiple Claude
 * sessions all share one server. Each conn slot holds a 256KB
 * write_buf, so 256 ≈ 64MB worst-case (within the unit's 2G high). */
#define SERVER_MAX_CONNECTIONS 256
#define SERVER_MAX_MSG_SIZE    (16 * 1024 * 1024) /* 16MB */
#define SERVER_READ_BUF_SIZE   65536              /* initial per-connection read buffer */
#define SERVER_WRITE_BUF_SIZE  262144             /* 256KB */
#define SERVER_DEFAULT_SOCKET  "aimee.sock"
#define SERVER_TOKEN_FILE      "server.token"
#define SERVER_TOKEN_LEN       64 /* 64 hex bytes = 32 raw bytes */
/* Listen backlog: number of connections the kernel will queue between
 * accept() calls. When the queue is full, new connect() calls fail with
 * ECONNREFUSED — observable as "aimee: server unavailable" from CLI
 * invocations, particularly bursty PreToolUse hooks across multiple
 * Claude sessions. 16 was too small once long-lived MCP connections,
 * webchat, and per-Bash hook calls all share one server. 128 matches
 * the historical POSIX SOMAXCONN; the kernel caps to net.core.somaxconn
 * (4096 on modern Linux), so this is plenty of headroom without
 * overcommitting memory in the kernel queue. */
#define SERVER_LISTEN_BACKLOG  128
#define CONN_WRITE_DEADLINE_MS 10000 /* 10 seconds */

/* Per-method payload size limits */
#define LIMIT_MEMORY   (256 * 1024)      /* 256KB for memory operations */
#define LIMIT_TOOL     (4 * 1024 * 1024) /* 4MB for tool I/O */
#define LIMIT_DELEGATE (4 * 1024 * 1024) /* 4MB: supports 2MB prompt-file + JSON overhead */
#define LIMIT_CHAT     (512 * 1024)      /* 512KB for chat messages */
#define LIMIT_INGEST   (1024 * 1024)     /* 1MB: client-pushed code files (kb req cap) */
#define LIMIT_DEFAULT  (256 * 1024)      /* 256KB default */

/* JSON framing limits */
#define JSON_MAX_DEPTH          32  /* maximum nesting depth */
#define JSON_MAX_FIELDS         256 /* maximum fields per object */
#define SERVER_SESSION_POOL_MAX 128
#define SERVER_SESSION_ID_MAX   128

/* Capability flags (bitmask) */
#define CAP_CHAT           (1u << 0)
#define CAP_DELEGATE       (1u << 1)
#define CAP_TOOL_EXECUTE   (1u << 2)
#define CAP_TOOL_BASH      (1u << 3)
#define CAP_TOOL_WRITE     (1u << 4)
#define CAP_MEMORY_READ    (1u << 5)
#define CAP_MEMORY_WRITE   (1u << 6)
#define CAP_RULES_READ     (1u << 7)
#define CAP_RULES_ADMIN    (1u << 8)
#define CAP_DESCRIBE_READ  (1u << 9)
#define CAP_DESCRIBE_ADMIN (1u << 10)
#define CAP_INDEX_READ     (1u << 11)
#define CAP_INDEX_ADMIN    (1u << 12)
#define CAP_SESSION_READ   (1u << 13)
#define CAP_SESSION_ADMIN  (1u << 14)
#define CAP_DASHBOARD_READ (1u << 15)

/* Composite capability sets */
#define CAPS_ALL 0xFFFFu
#define CAPS_READ_ONLY                                                                             \
   (CAP_CHAT | CAP_MEMORY_READ | CAP_RULES_READ | CAP_INDEX_READ | CAP_SESSION_READ |              \
    CAP_DASHBOARD_READ | CAP_DESCRIBE_READ)
#define CAPS_AUTHENTICATED                                                                         \
   (CAPS_READ_ONLY | CAP_DELEGATE | CAP_TOOL_EXECUTE | CAP_TOOL_BASH | CAP_TOOL_WRITE |            \
    CAP_MEMORY_WRITE | CAP_RULES_ADMIN | CAP_SESSION_ADMIN)

/* aimee.api.remote_writes levels: how far an authorized TCP bearer may go. The
 * UDS path is always full (CAPS_ALL); these gate the optional TCP listener.
 *   OFF  — mutating /v1 routes are local-UDS-only (default; leaked-bearer safe).
 *   DATA — data-mutating routes (memory.store, work.*, rules.delete, skill.*, …)
 *          allowed over TCP, gated by the per-route capability matrix.
 *   FULL — TCP bearer is fully trusted (CAPS_ALL): data writes AND the
 *          delegate/tool/bash methods over /v1. Trusted networks only. */
#define SERVER_REMOTE_WRITES_OFF  0
#define SERVER_REMOTE_WRITES_DATA 1
#define SERVER_REMOTE_WRITES_FULL 2

/* Method-to-capability policy entry */
typedef struct
{
   const char *method;      /* exact match, or prefix with trailing '*' */
   uint32_t required_caps;  /* capability bitmask (0 = no auth required) */
   const char *description; /* human-readable for audit/logging */
} method_policy_t;

/* Declarative method registry (defined in server_auth.c) */
extern const method_policy_t method_registry[];
extern const int method_registry_count;

/* Per-connection state.
 *
 * Concurrency model: the event-loop thread (server_run) owns reads —
 * it pulls bytes off the socket and parses newline-framed messages.
 * Each parsed message is enqueued onto the ephemeral pool. Ephemeral
 * worker threads run the handler and write the response. So multiple
 * threads can touch a conn:
 *
 *   - event loop reads + closes conn
 *   - ephemeral workers run handlers + write responses
 *
 * `mutex` serialises mutable connection state shared by the event loop
 * and workers (fd/closing/refcount/capabilities/write_buf/write_len/pos,
 * write_deadline_ms). Workers do not hold it while executing handlers;
 * they take it only around connection-state checks and response writes.
 *
 * `refcount` counts in-flight ephemeral jobs that hold a pointer to
 * this conn. conn_close marks `closing=1` and removes the fd from the
 * event loop without waiting; the event loop later reclaims the slot
 * once refcount reaches 0. Connection slots are fixed in place while
 * in use, so worker-held pointers stay valid for their job lifetime. */
typedef struct
{
   int in_use; /* slot is reserved by an open or closing connection */
   int fd;
   platform_evloop_t *evloop; /* pointer to server's event loop for OUT registration */
   uid_t peer_uid;
   gid_t peer_gid;
   pid_t peer_pid;
   uint32_t capabilities;
   char *read_buf;
   size_t read_cap;
   size_t read_len;
   char write_buf[SERVER_WRITE_BUF_SIZE];
   size_t write_len;          /* number of pending bytes in write_buf */
   size_t write_pos;          /* offset of first pending byte in write_buf */
   int64_t write_deadline_ms; /* monotonic ms when pending write must complete, 0 = no pending */
   pthread_mutex_t mutex;     /* serialises worker / event-loop access */
   pthread_cond_t can_close;  /* signalled when refcount drops to 0 */
   int refcount;              /* in-flight ephemeral jobs holding this conn */
   int closing;               /* set by conn_close once teardown is committed */
   char active_verify_session[SERVER_SESSION_ID_MAX]; /* sync git.verify cancellation key */
} server_conn_t;

typedef struct
{
   int initialized;
   int close_requested;
   int shutdown_started;
   int active_jobs;
   char session_id[SERVER_SESSION_ID_MAX];
   compute_pool_t pool;
} server_session_pool_t;

/* Server context */
typedef struct
{
   int listen_fd;
   platform_evloop_t evloop;
   char socket_path[4096];
   char token[SERVER_TOKEN_LEN * 2 + 1]; /* hex string */
   server_conn_t conns[SERVER_MAX_CONNECTIONS];
   int conn_count;
   pthread_mutex_t conns_mutex; /* serialises slot allocation/free and conn_count */
   compute_pool_t pool;
   compute_pool_t request_pool;
   int request_pool_initialized;
   pthread_mutex_t session_pools_mutex;
   int session_pools_initialized;
   int session_threads;
   server_session_pool_t session_pools[SERVER_SESSION_POOL_MAX];
   pthread_mutex_t compute_budget_mutex;
   pthread_cond_t compute_budget_cond;
   int compute_budget_total;
   int compute_budget_available;
   volatile int running;
   time_t start_time;
   int active_sessions; /* refcount of live sessions (atomic) */
   time_t
       last_session_end; /* when active_sessions last hit 0; 0 = sessions active or fresh start */

   /* Provider concurrency slots: global active count per agent, shared across
    * all CLI delegate processes that contact this server. Acquire/release via
    * the provider.slot_acquire / provider.slot_release RPCs. The mutex is
    * separate from conns_mutex to avoid contention with connection management.
    * Resets to zero on every server start — no stale slots survive a restart. */
   pthread_mutex_t provider_slots_mutex;
   char provider_slot_names[CATALOG_MAX_ENTRIES][64];
   int provider_slot_active[CATALOG_MAX_ENTRIES];
   int provider_slot_count;
} server_ctx_t;

/* Lifecycle */
int server_init(server_ctx_t *ctx, const char *socket_path);
int server_run(server_ctx_t *ctx);
void server_shutdown(server_ctx_t *ctx);
int server_compute_budget_acquire(server_ctx_t *ctx);
void server_compute_budget_release(server_ctx_t *ctx, int granted);
int server_session_pool_submit(server_ctx_t *ctx, const char *session_id, void (*fn)(void *),
                               void *arg, int *thread_count_out);
void server_session_pool_close(server_ctx_t *ctx, const char *session_id);
void server_session_pools_shutdown(server_ctx_t *ctx);
char *server_session_pools_json(server_ctx_t *ctx);

/* Method dispatch */
int server_dispatch(server_ctx_t *ctx, server_conn_t *conn, const char *msg, size_t msg_len);

/* Process-wide server context (server_main.c), for in-process dispatch. */
server_ctx_t *server_active_ctx(void);

/* Response helpers (shared across handler files) */
int server_send_response(server_conn_t *conn, cJSON *resp);
int server_send_error(server_conn_t *conn, const char *message, const char *request_id);

/* Declared from cJSON.h so the inline below needs no cJSON.h include here; the
 * real declaration is identical, so including both is harmless. */
void cJSON_Delete(cJSON *item);

/* Send `resp`, then free it, returning the send rc. Ownership-taking tail that
 * RPC handlers repeat after building their response object. Inline so it
 * resolves against whichever server_send_response is linked (real or a test
 * stub), with no new link dependency. */
static inline int server_send_ok(server_conn_t *conn, cJSON *resp)
{
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* Auth (server_auth.c) */
/* Constant-time string equality: returns 1 if equal, 0 otherwise, without
 * leaking length/content via timing. Used for the HTTP bearer comparison
 * (server_http_authorize). */
int server_ct_equal(const char *a, const char *b);
uint32_t server_capability_for_method(const char *method);
const method_policy_t *server_policy_for_method(const char *method);

/* Session handlers (server_session.c) */
int handle_session_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_brief(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_attach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_detach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_presence(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trajectory_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trajectory_batch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* State handlers (server_state.c) */
int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_read(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_benchmark(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_scan(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_credentials(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_structure(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_find_callers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_graph_sync_code(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_graph_explain(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_implements(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_synthesize(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_contradictions(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_invalidated(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_build(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_update(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_docs_push(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_ingest_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_optimize_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_optimize_promote(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_calibration_readiness(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_demotion_check(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_optimize_replay_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_rules_generate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_rules_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_collab_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_collab_rules_list_active(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_collab_rules_approve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_collab_rules_reject(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_collab_rules_retire(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_wm_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_primary_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_primary_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_primary_clear(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_add_batch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_claim(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_complete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_fail(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_board(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_release(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_clear(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_gc(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_sync_proposals(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_work_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_traces(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_plans(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_plugins(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_plugin_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_plugin_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_plugin_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_onboard(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_lsp_diagnostics_summary(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_all(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
char *server_agent_list_json(void);
int handle_workspace_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workspace_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workspace_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workspace_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workspace_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_workspace_mirror_sync(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_runner_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_runner_respond(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dogfood_tag(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dogfood_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dogfood_report(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_identity_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_identity_snapshot(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_identity_diff(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Compute handlers (server_compute.c) */
int handle_tool_execute(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_aggregate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_roundtable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_launch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_jobs_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_jobs_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_jobs_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_jobs_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_coord_job_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_coord_job_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_coord_job_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_coord_job_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_aux_config_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_config_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_config_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_config_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_aux_test(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_log(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_episode_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_episodes(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_chat_send_stream(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
void server_compute_async_drain(void);
cJSON *server_compute_async_json(server_ctx_t *ctx);

/* Agent management handlers (server_agent.c) */
int handle_agent_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_local(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_probe(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_setup(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_setup_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* MCP proxy handler (server_mcp.c) */
int handle_mcp_tools_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_mcp_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_mcp_recheck(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_resolve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

#endif /* DEC_SERVER_H */
