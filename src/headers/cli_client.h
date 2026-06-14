#ifndef DEC_CLI_CLIENT_H
#define DEC_CLI_CLIENT_H 1

#include <stddef.h>
#include <signal.h>

#define CLIENT_DEFAULT_TIMEOUT_MS 5000
#define CLIENT_CONNECT_TIMEOUT_MS 1000
#define CLIENT_READ_BUF_SIZE      (256 * 1024) /* 256KB */
#define CLIENT_MAX_RESPONSE_SIZE  (16 * 1024 * 1024)

/* Forward declaration -- cJSON.h included by .c files */
typedef struct cJSON cJSON;

/* Connection handle */
typedef struct
{
   int fd;
   char read_buf[CLIENT_READ_BUF_SIZE];
   size_t read_len;
} cli_conn_t;

/* Connect to server. Returns 0 on success, -1 on failure.
 * socket_path may be NULL for default (~/.config/aimee/aimee.sock). */
int cli_connect(cli_conn_t *conn, const char *socket_path);

/* Send a JSON request and receive a JSON response.
 * Caller owns the returned cJSON object (must cJSON_Delete).
 * Returns NULL on error (timeout, disconnect, parse failure). */
cJSON *cli_request(cli_conn_t *conn, cJSON *request, int timeout_ms);

/* Callback for streaming events. Called for each intermediate JSON message
 * that has an "event" field. Return 0 to continue, non-zero to abort. */
typedef int (*cli_stream_cb)(cJSON *event, void *userdata);

/* Check if server is available (GET /v1/health over the local HTTP UDS).
 * Returns 1 if available, 0 if not. The socket_path argument is ignored (the
 * co-located server is reached over aimee-http.sock). */
int cli_server_available(const char *socket_path);

/* Run a unary {method,...} dispatch by resolving the method to its first-class
 * /v1 route and sending it to the co-located server (local aimee-http.sock on
 * POSIX; the configured remote on Windows). Returns the parsed dispatch response
 * (caller cJSON_Delete()s it), or NULL on transport failure or when the method
 * has no /v1 route. The local UDS is filesystem-trusted (server_dispatch still
 * enforces per-method caps); dispatch-level errors come back as a
 * {status:"error",...} body. Replaces the legacy NDJSON
 * connect/authenticate/request sequence with first-class /v1 dispatch routes. */
cJSON *cli_v1_dispatch_local(cJSON *req, int timeout_ms);

/* ── /v1 HTTP transport (aimee.api.client_transport) ───────────────────────
 * First-party clients can reach aimee-server over its /v1 HTTP surface
 * (UDS or 127.0.0.1:port) instead of the private NDJSON socket. */

/* How a client should reach the server. */
typedef enum
{
   CLI_TRANSPORT_SOCKET = 0, /* legacy NDJSON Unix socket (default) */
   CLI_TRANSPORT_HTTP,       /* /v1 HTTP surface only */
   CLI_TRANSPORT_AUTO,       /* prefer HTTP, fall back to the socket */
} cli_transport_t;

/* Map a config string ("socket"/"http"/"auto") to a cli_transport_t. NULL or
 * empty or any unrecognized value ⇒ CLI_TRANSPORT_SOCKET (safe default). */
cli_transport_t cli_transport_parse(const char *s);

/* First-class /v1 REST route for a dispatch method whose server route is
 * dispatch-backed (rh_dispatch_op), so an HTTP call returns byte-identical
 * results to the NDJSON socket. Returns the request path and sets *verb_out
 * ("GET"/"POST"); returns NULL (with *verb_out defaulted to "POST") when the
 * method has no parity-safe REST route. Only POST routes and no-argument GET
 * reads are mapped. */
const char *cli_v1_route_for_method(const char *method, const char **verb_out);

/* {id}-bearing /v1 routes (PREFIX{id}SUFFIX, e.g. /v1/workspaces/{path},
 * /v1/sessions/{id}/attach). Returns the static prefix and fills the verb, the
 * suffix after {id}, and the request field carrying the id ("session_id"; NULL
 * means the first positional arg). NULL if `method` is not a path-id route. */
const char *cli_v1_pathid_route_for_method(const char *method, const char **verb_out,
                                           const char **suffix_out, const char **id_field_out);

/* Percent-encode `in` into `out` (cap incl. NUL) for a path-id URL segment;
 * 0 on success, -1 on overflow. */
int cli_v1_pct_encode(const char *in, char *out, size_t cap);

/* Build a minimal HTTP/1.1 request into buf[cap]:
 *   "<method> <path> HTTP/1.1\r\nHost: <host>\r\n"
 *   ["Authorization: Bearer <bearer>\r\n"]
 *   "Content-Type: application/json\r\nContent-Length: <n>\r\n"
 *   "Connection: close\r\n\r\n<body>"
 * host defaults to "localhost" when NULL/empty; the Authorization header is
 * omitted when bearer is NULL/empty; body may be NULL (Content-Length: 0).
 * Returns the total byte length written (excluding the NUL), or -1 if it does
 * not fit in cap. */
int cli_http_build_request(const char *method, const char *path, const char *host,
                           const char *bearer, const char *body, char *buf, size_t cap);

/* Issue an HTTP/1.1 request to aimee-server's /v1 surface and return the parsed
 * JSON response body (caller cJSON_Delete()s it). `endpoint` is a UDS path
 * ("/run/aimee/http.sock" or "unix:<path>") or a TCP target ("host:port" or
 * "tcp:host:port"). `method`/`path` are the HTTP verb and request target
 * (e.g. "GET", "/v1/models"); `body_json` is an optional request body; `bearer`
 * is sent as Authorization when non-empty. On success the parsed body is
 * returned and *http_status (when non-NULL) is set to the HTTP status code.
 * Returns NULL on connect / transport / parse failure (and sets *http_status to
 * 0); a non-2xx response still returns the parsed error body with *http_status
 * set, so callers can distinguish transport failure from an application error. */
cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body_json, const char *bearer, int timeout_ms,
                        int *http_status);

/* Streaming variant of cli_http_request(): parses a Server-Sent Events response,
 * invoking cb() per event. Returns the last event (caller frees) or NULL. */
cJSON *cli_http_request_stream(const char *endpoint, const char *method, const char *path,
                               const char *body_json, const char *bearer, int timeout_ms,
                               int *http_status, cli_stream_cb cb, void *userdata);

/* NDJSON streaming variant: the response body is the native aimee event stream
 * (one JSON object per line; intermediate events carry "event", the final
 * message carries "status"). cb() is invoked per intermediate event (nonzero
 * aborts); the final "status" object is returned (caller frees). on_open(fd,ud),
 * when non-NULL, is called with the live socket fd right after connect (and with
 * -1 just before close) so an external thread can abort the turn by closing it.
 * Used to consume POST /v1/chat/stream. Returns NULL on transport failure. */
cJSON *cli_http_request_stream_ndjson(const char *endpoint, const char *method, const char *path,
                                      const char *body_json, const char *bearer, int timeout_ms,
                                      int *http_status, cli_stream_cb cb, void *userdata,
                                      void (*on_open)(int fd, void *ud), void *on_open_ud);

/* Close connection. */
void cli_close(cli_conn_t *conn);

/* Connect with a custom timeout (ms). socket_path may be NULL for default. */
int cli_connect_timeout(cli_conn_t *conn, const char *socket_path, int timeout_ms);

/* Last low-level socket connection failure observed by cli_connect_timeout().
 * Used only for diagnostics after cli_ensure_server* returns NULL. */
int cli_last_connect_errno(void);
const char *cli_last_connect_path(void);
int cli_connect_errno_is_permission_denied(int err);

/* Default socket path: ~/.config/aimee/aimee.sock */
const char *cli_default_socket_path(void);

/* Ensure a server is running. Tries AIMEE_SOCK, then well-known socket,
 * then auto-starts a server. Sets active_socket_path.
 * Returns the socket path to use, or NULL on failure. */
const char *cli_ensure_server(void);
const char *cli_ensure_server_for_method(const char *method);

/* cli_workspace_serve.c: run the client-side detached-workspace runner serve
 * loop (poll -> execute locally -> respond) until interrupted. Returns 0 on a
 * clean stop, non-zero on a usage / connection error. */
int cmd_workspace_serve(const char *workspace_id);

/* The serve loop without signal handling, for callers that drive it on a
 * background thread with their own stop flag (e.g. mcp-serve's reverse-channel).
 * Either `sock` (local) or `endpoint`+`bearer` (remote) selects the transport. */
int cli_workspace_serve_loop(const char *workspace_id, const char *sock, const char *endpoint,
                             const char *bearer, volatile sig_atomic_t *stop);

/* Reverse-channel for interactive/bridge commands (mcp-serve, chat) against a
 * remote aimee-server: register the client's cwd as a `detached` workspace and
 * serve it on a background thread so the server's file/exec tools route back to
 * this client. start() returns 1 if a channel was started (remote configured),
 * 0 otherwise (incl. co-located). stop() tears it down. One channel per process. */
int cli_workspace_reverse_channel_start(void);
void cli_workspace_reverse_channel_stop(void);

/* Return the path to an already-running compatible server, or NULL if none
 * is available. Unlike cli_ensure_server(), this does not auto-start. */
const char *cli_existing_server(void);
const char *cli_existing_server_for_method(const char *method);

/* Explicitly terminate the running aimee-server (if any) and spawn a
 * fresh one. This is the user-invoked path; the diagnostic try_server
 * path no longer signals the peer. Returns 0 on success, non-zero on
 * failure. */
int cli_restart_server(void);

/* Spawn aimee-server if it's not already running. The CLI no longer
 * auto-spawns on demand; this is the explicit one-shot for boxes
 * where systemd / launchd / SCM isn't the lifecycle owner. Returns 0
 * if a server is up (already-running OR newly-spawned), non-zero on
 * spawn failure. */
int cli_start_server(void);

/* RPC thin-client routing.
 * Looks up an RPC route for a CLI command+subcommand combination. */
typedef struct
{
   const char *method;        /* logical route/formatter name */
   const char *server_method; /* actual server RPC method (NULL = method) */
   const char *extract;       /* response field to extract (NULL = return object minus "status") */
   int skip_subcmd;           /* number of leading sub-args consumed by route match (0, 1, or 2) */
   int timeout_ms;            /* RPC timeout (0 = CLIENT_DEFAULT_TIMEOUT_MS) */
} cli_rpc_route_t;

/* Returns 1 if a matching RPC route was found, 0 otherwise.
 * Tries compound sub-commands first (e.g. "ingest status") before single ones. */
int cli_rpc_lookup(const char *cmd, int sub_argc, char **sub_argv, cli_rpc_route_t *route);

/* Forward a CLI command through the server RPC.
 * Returns 0 on success, >0 on application error, -1 on transport/protocol
 * error (caller should fall back to in-process execution).
 * argc/argv are the args AFTER the command name (e.g., for "aimee memory search foo",
 * argv = ["search", "foo"]). The route's skip_subcmd controls whether the first
 * arg is stripped before marshaling. */
int cli_rpc_forward(const char *socket_path, const cli_rpc_route_t *route, int json_output,
                    const char *json_fields, const char *response_profile, int argc, char **argv);

/* True when the thin client is configured to reach a remote aimee-server /v1
 * endpoint (client_transport != socket AND AIMEE_API_ENDPOINT /
 * aimee.api.client_endpoint set). Callers skip the local-socket preflight. */
int cli_rpc_has_remote_endpoint(void);

/* True only when the configured endpoint is a remote "tcp:host:port" (not a
 * local "unix:" path). Interactive commands (chat/launch) refuse a remote
 * endpoint because the agent/tools/worktree run on the client host. */
int cli_rpc_remote_endpoint_is_tcp(void);

/* Resolve the remote /v1 endpoint ("tcp:host:port" / "unix:path") and bearer for
 * the HTTP transport (env AIMEE_API_ENDPOINT / AIMEE_API_BEARER, else aimee.yaml
 * client_endpoint / bearer_token). Caller frees. NULL when unset. POSIX only. */
char *cli_rpc_client_endpoint(void);
char *cli_rpc_client_bearer(void);

/* Thin-client workspace push: when the configured endpoint is a remote
 * "tcp:host:port" the server cannot see this host's filesystem, so
 * `aimee workspace add <path>` resolves the path locally, registers it as a
 * `detached` workspace, and pushes the file contents to POST /v1/index/ingest;
 * `aimee index scan [path]` re-pushes (all detached workspaces when no path).
 * Return 0 on success. POSIX only (no-op error on Windows). */
int cli_workspace_add_remote(const char *path);
int cli_index_scan_remote(int argc, char **argv);

/* Attach this machine's Codex OAuth creds (~/.codex/auth.json) to an outbound
 * request body when talking to a remote tcp server, so the server can auth a
 * codex-oauth agent without holding any codex credential. No-op otherwise. */
void cli_attach_codex_creds(cJSON *req);

/* Launch metadata parsed from server output */
typedef struct
{
   char provider[64];
   char model[128];
   char session_id[64];
   int builtin;
   int autonomous;
   char worktree_cwd[4096];
   size_t context_len; /* bytes of session context before __LAUNCH__ marker */
} launch_meta_t;

/* Parse __LAUNCH__ metadata from server output.
 * Returns 1 if launch metadata was found and parsed, 0 otherwise. */
int parse_launch_meta(const char *output, launch_meta_t *meta);

/* Client-local command handlers (run in the client so they see its working
 * tree). Defined in cmd_profile.c / cmd_manuscript.c. */
int cmd_profile_run(int argc, char **argv);
int cmd_manuscript_run(int argc, char **argv, int json_output);
/* `aimee optimize` — bandit optimization loop (points/baseline/replay).
 * Dispatches optimize.export to GET /v1/optimize/export. Defined in cmd_optimize.c. */
int cmd_optimize_run(int argc, char **argv, int json_output);

/* `aimee persona <list|show|edit|add|rm>` — manage personas over the server's
 * /v1 HTTP API. Defined in cli_persona.c. */
int cmd_persona_client_run(int argc, char **argv, int json_output);

/* `aimee workflow <blocks|validate|show|list|new>` — inspect/validate workflow
 * definitions locally. Defined in cmd_workflow.c. */
int cmd_workflow_client_run(int argc, char **argv, int json_output);

/* `aimee roles <list|show|edit|rm>` — manage delegate role templates over the
 * server's /v1 HTTP API. Defined in cli_roles.c. */
int cmd_roles_client_run(int argc, char **argv, int json_output);

#endif /* DEC_CLI_CLIENT_H */
