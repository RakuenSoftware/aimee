#ifndef DEC_SERVER_H
#define DEC_SERVER_H 1

#include "aimee_features.h"
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include "compute_pool.h"
#include "platform_event.h"
#include "provider_catalog.h"
#include "index.h" /* project_info_t, for server_active_project_match */

/* True when `name` is a provider the chat path can resolve: a built-in CLI
 * provider, a known adapter, or an agent in `acfg` (matched by name or by
 * provider). Gates the durable primary write in handle_provider_set so a
 * mistyped `aimee provider <word>` cannot brick every later chat turn.
 * `acfg` may be NULL to check only the built-in/adapter sets. */
int provider_name_settable(const char *name, const agent_config_t *acfg);
#include "vault_principal.h"

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

/* The largest /v1 request body the HTTP listener will accept (the roundtable
 * review path has its own, larger cap). Lives here rather than inside
 * server_http.c so a CLIENT can refuse an oversized request itself and say why:
 * a body over this is dropped by the listener, which the client could otherwise
 * only report as "could not reach the endpoint" — blaming a server that is up
 * and answering. The sibling LIMIT_* values below are already documented against
 * it. */
#define SHTTP_MAX_BODY (4 * 1024 * 1024)

/* The roundtable review artifact is hard-limited where the CLI reads it --
 * marshal_read_stdin_limited / marshal_read_file_limited in cli_v1_routes.c, all
 * three call sites -- so a review body is that artifact plus a small envelope.
 *
 * THE ARTIFACT IS BOUNDED BY THE REVIEWING MODEL, NOT BY THE WIRE. It is the
 * thing being read, so an artifact bigger than the context that has to hold it
 * cannot be reviewed -- it is truncated or refused downstream, and accepting it
 * here only moves the failure later. A 1M-token context holds roughly 3-4MB of
 * code (code tokenizes at about 3-3.5 chars/token), so 8MB is already twice the
 * largest artifact that can be read, with room for tokenizer variance and
 * multi-byte UTF-8. 16MB was inherited from 842ff35656 ("preserve exact review
 * artifacts"), a Go-side change that touched the C client limit in passing; it
 * had no recorded rationale.
 *
 * For scale on this repo: the largest single source file is 0.25MB, and EVERY
 * .c and .h in src/ concatenated is 32.8MB -- larger than even the 16MB limit
 * this replaces, so "review the whole tree at once" never fit either way.
 *
 * The transport cap is TWICE the artifact: real review text and diffs escape at
 * about 1.02x, and the doubling covers the envelope plus quote/backslash-dense
 * content with room to spare.
 *
 * It was 128MB. That assumed a 6x blowup -- every byte a control character
 * escaping to \u00XX -- and rounded up to 2^27, which made this route accept 32x
 * what the rest of /v1 does. The artifact reaches cJSON as a NUL-terminated
 * string, so the all-control-bytes case it was sized for cannot arrive intact.
 * If an escape-dense artifact ever genuinely needs more room, raise
 * ROUNDTABLE_MAX_ARTIFACT and let the cap follow; do not re-inflate the
 * transport limit on its own, because the listener allocates against it. */
#define ROUNDTABLE_MAX_ARTIFACT   (8 * 1024 * 1024)
#define SHTTP_MAX_ROUNDTABLE_BODY (2 * ROUNDTABLE_MAX_ARTIFACT)

/* Per-method payload size limits */
#define LIMIT_MEMORY     (256 * 1024)        /* 256KB for memory operations */
#define LIMIT_TOOL       (4 * 1024 * 1024)   /* 4MB for tool I/O */
#define LIMIT_DELEGATE   (4 * 1024 * 1024)   /* 4MB: supports 2MB prompt-file + JSON overhead */
#define LIMIT_ROUNDTABLE SHTTP_MAX_ROUNDTABLE_BODY /* artifact + JSON escaping; see above */
#define LIMIT_CHAT       (512 * 1024)        /* 512KB for chat messages */
#define LIMIT_INGEST     (1024 * 1024)       /* 1MB: client-pushed code files (kb req cap) */
#define LIMIT_TRANSCRIPT                                                                           \
   (3 * 1024 * 1024)               /* 3MB: session transcript snapshots (< SHTTP_MAX_BODY) */
#define LIMIT_DEFAULT (256 * 1024) /* 256KB default */

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
/* Operator-level: approve/reject autonomous-workflow human gates. Deliberately
 * OUTSIDE CAPS_AUTHENTICATED (full-trust / UDS / webchat-admin only), so a mere
 * authenticated/delegate bearer cannot drive a human gate. */
#define CAP_WORKFLOW_ADMIN (1u << 16)

/* Operator-level: register/unregister shadow-traffic subscribers (a subscriber
 * receives a copy of every completion request, i.e. all prompt/response content).
 * Deliberately OUTSIDE CAPS_AUTHENTICATED (full-trust / UDS / remote_writes=full
 * only), so a mere authenticated bearer cannot tap live traffic. */
#define CAP_SHADOW_ADMIN (1u << 17)

/* Operator-level: administer per-user /v1 write-tier grants — who may write to which
 * remote server. Deliberately OUTSIDE CAPS_AUTHENTICATED for the same reason as the two
 * above, and the reason is sharper here: a bearer that could administer grants could grant
 * ITSELF a higher tier, which would make the whole tier system decorative.
 *
 * This is defence in depth and not the primary control. The primary control is
 * v1_route_requires_uds, which refuses these routes over TCP regardless of capability —
 * necessary because a remote_writes=full bearer holds CAPS_ALL and would otherwise satisfy
 * any capability check. kb then independently requires admin or team-lead authority. */
#define CAP_GRANT_ADMIN (1u << 18)

/* Composite capability sets */
#define CAPS_ALL 0x7FFFFu
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
   /* WP-C.0: attested identity for the credential vault, captured while the conn
    * is live and propagated across the loopback_rpc fake-conn boundary. The
    * vault principal ("uid:<n>" / "webuser:<name>" / "" when un-attested) is the
    * single security key for the vault file + KEK cache — derived from the
    * kernel-attested peer_uid or root-UDS-gated webuser assertion, NEVER
    * a client-supplied session_id. Empty principal => no vault (fail-closed). */
   attested_transport_t attested_transport;
   char vault_principal[VAULT_PRINCIPAL_MAX];
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
   server_conn_t conns[SERVER_MAX_CONNECTIONS];
   int conn_count;
   pthread_mutex_t conns_mutex; /* serialises slot allocation/free and conn_count */
   compute_pool_t pool;
   compute_pool_t request_pool;
   int request_pool_initialized;
   compute_pool_t orchestration_pool;
   int orchestration_pool_initialized;
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

} server_ctx_t;

/* Lifecycle */
int server_init(server_ctx_t *ctx, const char *socket_path);
int server_run(server_ctx_t *ctx);
/* True if an aimee-server instance is already running for `socket_path` (pid-file
 * + liveness check). Used by the offline --rotate-master-key guard (D13 F2). */
int server_is_running(const char *socket_path);
/* Boot-time credential provisioning: seal operator-supplied delegate API keys
 * and the first-boot AIMEE_FORGE_TOKEN into the server-principal Vault, then
 * scrub credential environment variables. No-op when no source is set; returns
 * the count provisioned. */
int server_vault_bootstrap(void);
int server_vault_bootstrap_prepare(void);
/* Resolve a delegate name to its canonical agents.json name: returns 1 and
 * writes `canon` (NUL-terminated, capped at `cap`) when known, else 0. The
 * provisioning module calls this through an injected pointer so it carries no
 * link dependency on the agent-config layer; production wires an agents.json
 * resolver, unit tests wire a trivial one. */
typedef int (*aimee_agent_resolver_fn)(const char *name, char *canon, size_t cap);
void server_vault_bootstrap_set_resolver(aimee_agent_resolver_fn fn);
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

/* Fault classes for server_send_error_kind. The runtime-web process maps these
 * over the event bus and the server adds its status to the dispatch envelope.
 * Optional and additive: an unclassified or unavailable decision remains a
 * generic 502 at the physical web boundary. */
#define SERVER_ERR_INVALID_ARGUMENT  "invalid_argument"  /* caller sent bad/missing input */
#define SERVER_ERR_NOT_FOUND         "not_found"         /* named thing does not exist */
#define SERVER_ERR_PERMISSION_DENIED "permission_denied" /* caller not allowed */
#define SERVER_ERR_UNAVAILABLE       "unavailable"       /* a dependency is down */

/* The typed error as a VALUE, for commands that RETURN a result rather than write
 * one. jo_err is not a substitute: it omits `kind` and the derived `http_status`,
 * so splitting an RPC handler through it downgrades a typed error to an untyped
 * one. Same function builds both forms, so they cannot drift. */
cJSON *server_error_kind_json(const char *kind, const char *message, const char *request_id);

int server_send_error_kind(server_conn_t *conn, const char *kind, const char *message,
                           const char *request_id);

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
int handle_session_record_transcript(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_chat_graceful_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_chat_interrupt(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_brief(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_attach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_detach(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_session_presence(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trajectory_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trajectory_batch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* State handlers (server_state.c) */
int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* Resolve request-local project/workspace memory identity and activate it for
 * subsequent kb_client memory calls on this worker thread. Returns 1 when the
 * active identity is missing; caller must clear the client context. */
int server_memory_scope_begin(cJSON *req);
int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* The same command in the shape the core command table routes: takes arguments,
 * RETURNS the result, writes to no connection. The handler above is now only the
 * RPC surface's connection write. This is the shape every surface needs, and the
 * lack of it is why capability surface was declared four separate times. */
cJSON *memory_store_command(const cJSON *req);
cJSON *memory_list_command(const cJSON *req);
cJSON *memory_get_command(cJSON *req);
cJSON *memory_delete_command(cJSON *req);
int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_supersede(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_read(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_benchmark(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_scan(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_structure(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_span(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_investigate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_hybrid(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_find_callers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_index_deps(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_graph_sync_code(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_graph_explain(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_evidence_trace(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_evidence_provenance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_evidence_fidelity(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_css_signals(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_implements(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_synthesize(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_contradictions(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_curator_invalidated(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_build(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_update(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_docs_push(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_reembed(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_memory_embed(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_ingest_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_kb_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_optimize_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_optimize_promote(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_calibration_readiness(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_demotion_check(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_ranker_export_view(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_ranker_fit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
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
int handle_hosts_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_embedders_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_primary_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_primary_clear(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_economizer_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_traces(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_plans(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_onboard(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_lsp_diagnostics_summary(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_all(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_dashboard_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_audit_verify(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_audit_captures(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_audit_replay(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_audit_checkpoint(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_audit_seal(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_audit_snapshot(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
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
int handle_roundtable_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_reservation_forget(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_cancel_unassigned(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* Deepening sweep (Part B): analysis-only — proposes seams per area and re-grounds
 * each against the live code index; returns a JSON report. Files nothing. */
int handle_dev_sweep(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_launch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* Credential vault (WP-C.1): resolve the attested principal from the conn. */
int handle_vault_unlock(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_vault_rekey(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_vault_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_vault_set_server(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Emit a VAULT_SERVER_WRITE record to the dedicated append-only 0600 audit sink
 * (audit_log) for a server-principal credential write (agent, cred type,
 * non-secret fingerprint, attested transport, acting principal). Shared by every
 * server-vault write path so each one is logged identically — never logs the
 * secret itself. */
void vault_audit_server_write(const server_conn_t *conn, const char *agent, const char *cred,
                              const char *secret);

int handle_vault_capability(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_vault_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_vault_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_vault_lock(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* mTLS client-cert lifecycle (server_cert.c) */
int handle_cert_issue(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_cert_sign(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_cert_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_cert_revoke(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
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
int handle_config_deploy_env(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
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
/* Returns 0 only after the atomic agents.json replacement committed; every
 * nonzero result is proven pre-effect.  Used by the management action barrier. */
int server_agent_management_set_enabled(const char *name, int enabled);
int handle_agent_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_roles(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_personas(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_probe(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_draft(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_setup(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_setup_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_cli_oauth_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_cli_oauth_code(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_agent_cli_oauth_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* MCP proxy handler (server_mcp.c) */
int handle_mcp_tools_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* Full served tool surface (built-ins + discovery + server-only), unfiltered;
 * caller owns the returned cJSON array (server_mcp_tools.c). */
cJSON *mcp_build_full_served_list(void);
int handle_mcp_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_mcp_recheck(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_get_help(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Record the served-call verdict from a sub-handler that returned early (same
 * thread as handle_mcp_call). See server_mcp.c. */
void server_mcp_served_outcome(const char *verdict, const char *reason);
/* Resolve the caller's active project from the request `cwd`: the working tree
 * first (co-located callers keep their durable repo identity), else the longest
 * registered project root that covers the path. A remote thin client sends a
 * path this process cannot stat, so the root match is the only answer available
 * there. Returns 0 and fills `out` on success, -1 with `out` emptied otherwise —
 * callers must then emit the typed `scope_required` error. See
 * server_active_project.c. */
int server_active_project_from_cwd(const char *cwd, char *out, size_t outlen);
/* The root-matching half, split out so it is unit-testable without a live
 * knowledge service. */
int server_active_project_match(const char *cwd, const project_info_t *projects, int count,
                                char *out, size_t outlen);

int handle_toolset_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_toolset_resolve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

#endif /* DEC_SERVER_H */
