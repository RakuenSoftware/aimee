/* server_http.h: aimee-server inbound HTTP-over-UDS /v1 API.
 *
 * First slice of the aimee-server-http-api proposal: a small hand-rolled
 * HTTP/1.1 listener bound to a dedicated Unix socket, serving the /v1/personas
 * resource. New handlers write HTTP responses directly (no conn-transport
 * virtualization / OpenAI shaping — those are later phases). Mirrors the
 * aimee-kb HTTP server (src/kb/http/). */
#ifndef DEC_SERVER_HTTP_H
#define DEC_SERVER_HTTP_H 1

#include "aimee_features.h"
#include <stddef.h>
#include <stdint.h>

#define SERVER_HTTP_MGMT_PATH_MAX    4096
#define SERVER_HTTP_START_MGMT_FATAL (-2)

#ifdef __cplusplus
extern "C"
{
#endif

   struct cJSON;
   /* Shared autonomous-run intake, used by POST /v1/dev/submit and the
    * `workflow_run` MCP tool so both go through one capped/audited path.
    * Resolves the workflow (explicit `workflow_opt` wins; NULL ⇒ autoroute or
    * "build"), enforces per-principal concurrency+rate caps keyed on
    * `submitter`, creates+binds+audits the run atomically, persists the proposal
    * artifact, sets the per-run cost cap, and notifies the scheduler. On success
    * returns 200 and (when `out` is non-NULL) sets *out to a
    * {work_item_id, workflow, state} object the caller owns. On failure returns
    * an HTTP-style status (400/401/429/500/503) and fills `err`; *out stays NULL.
    * `proposal_md` is required; `repo` may be "" ; `submitter` must be non-empty. */
   int dev_submit_run(const char *proposal_md, const char *workflow_opt, const char *repo,
                      const char *submitter, struct cJSON **out, char *err, size_t errlen);

   typedef struct
   {
      int enabled;
      int port;
      uint32_t bind_addr;
      char bind[16];
      char cert[SERVER_HTTP_MGMT_PATH_MAX];
      char key[SERVER_HTTP_MGMT_PATH_MAX]; /* Vault runtime-secret name, never a path */
      char client_ca[SERVER_HTTP_MGMT_PATH_MAX];
      char status_endpoint[SERVER_HTTP_MGMT_PATH_MAX];
      char status_ca[SERVER_HTTP_MGMT_PATH_MAX];
      char status_leaf_pin[65];
      char status_secondary_leaf_pin[65];
      char status_client_cert[SERVER_HTTP_MGMT_PATH_MAX];
      char status_client_key[SERVER_HTTP_MGMT_PATH_MAX]; /* Vault runtime-secret name */
      char status_key_id[65];
      char status_public_key[65];
   } server_http_management_config_t;

   /* Parse the all-or-none dedicated-management packet. Public paths/metadata
    * come from env; both private keys must already have been ingested into the
    * process-local Vault cache. Legacy private-key path variables are rejected. */
   int server_http_management_config_from_env(server_http_management_config_t *out);
   const char *server_http_management_last_error(void);

   /* Pure strict helpers used by startup/request tests. */
   int server_http_management_bind_addr(const char *text, uint32_t *out);
   int server_http_management_framing_valid(const char *method, const char *path,
                                            const char *request, size_t request_len);
   int server_http_management_action_framing_valid(const char *method, const char *path,
                                                   const char *request, size_t request_len);

   /* Start the HTTP listener(s), spawning a detached accept-loop thread that
    * polls all bound sockets. The UDS at uds_path (unlinked first;
    * filesystem-permission auth, no token) is always bound. When tcp_port > 0
    * AND bearer_token is non-empty, a localhost (127.0.0.1) TCP listener is
    * also bound and gated by `Authorization: Bearer <bearer_token>`; a
    * configured port with no bearer is refused (UDS still binds). Returns 0 if
    * the UDS bound, -1 on error. Idempotent-safe: a second call while running
    * returns -1. A configured management-listener failure returns the distinct
    * SERVER_HTTP_START_MGMT_FATAL so the process can fail closed. rate_limit_per_min caps
    * authorized TCP requests per 60s
    * window (0 = unlimited); over-limit ⇒ 429 + Retry-After. */
   int server_http_start(const char *uds_path, int tcp_port, int tls_port, const char *bearer_token,
                         int rate_limit_per_min, int remote_writes);

   /* Pure bind-address decision for a TCP /v1 listener (no I/O — unit-testable).
    * want_external is 1 when AIMEE_SERVER_HTTP_BIND requests a 0.0.0.0 bind;
    * allow_external is 1 ONLY for the native-TLS listener. A plaintext listener
    * (allow_external == 0) is pinned to INADDR_LOOPBACK even when an external
    * bind is requested, so the bearer and credentials can never face the network
    * in cleartext. Returns INADDR_ANY only when (want_external && allow_external),
    * else INADDR_LOOPBACK (both in host byte order). */
   uint32_t server_http_resolve_bind_addr(int want_external, int allow_external);

   /* Pure authorization decision for one HTTP request (no I/O — unit-testable).
    *   is_tcp          : the request arrived on the TCP listener (bearer
    *                     required) vs the UDS (filesystem-permission auth).
    *   bearer_cfg      : the configured bearer token, or NULL/"" if none.
    *   auth_header     : the Authorization header value (e.g. "Bearer xyz"), or
    *                     NULL if absent.
    *   api_key_header  : the x-api-key header value, or NULL if absent.
    *   has_session_key : an X-Aimee-Session-Key header was present.
    * Returns 0 if authorized, else the HTTP status to reject with: 503 when a
    * session key is presented without a bearer configured, or when TCP is
    * reached with no bearer configured; 401 on a missing/invalid bearer over
    * TCP. UDS requests are always authorized (subject to the session-key rule).
    * The compare is constant-time. */
   int server_http_authorize(int is_tcp, const char *bearer_cfg, const char *auth_header,
                             const char *api_key_header, int has_session_key);

   /* JSON body for a server_http_authorize rejection (pure — unit-testable).
    * |az| is that function's non-zero return. The 401 text carries the recovery
    * path for a bearer rotation, which invalidates every already-paired client
    * at once; clients echo this text verbatim, so it is the only guidance most
    * operators will see. */
   const char *server_http_auth_error_body(int az);

   /* As server_http_authorize, but accepts any of |extra| alongside the primary
    * bearer. This is what lets a client pair without revoking the credential
    * every other client is already using. Compares every candidate regardless of
    * an early match so timing does not reveal which token matched. */
   int server_http_authorize_multi(int is_tcp, const char *bearer_cfg, const char *const *extra,
                                   int extra_count, const char *auth_header,
                                   const char *api_key_header, int has_session_key);

   /* Publish the additional accepted bearers to the live listener. */
   void server_http_set_bearer_extra(const char *const *bearers, int n);

   /* How many additional bearers are currently accepted (diagnostics/tests). */
   int server_http_enrolled_bearer_count(void);

   /* server_http_authorize_multi against the live enrolled set. */
   int server_http_authorize_enrolled(int is_tcp, const char *bearer_cfg, const char *auth_header,
                                      const char *api_key_header, int has_session_key);
   int server_http_authorize_enrolled_request(int is_tcp, const char *bearer_cfg,
                                              const char *auth_header, const char *api_key_header,
                                              int has_session_key, int *bootstrap_only);

   /* Provision the authenticated setup-wizard user as the appliance's first
    * remote owner.  The returned bearer is enrollment-only until /v1/cert/sign
    * binds it to the client's CSR-produced mTLS certificate.  Returns 0 with a
    * bearer ready, 1 when that owner is already paired, -2 when another user
    * owns the appliance, or -1 on validation/storage/config failure. */
   int server_http_first_user_bootstrap(const char *principal, char *bearer, size_t bearer_cap);

   /* Complete and resolve the explicit first-user certificate grant. */
   int server_http_first_user_bind_cert(const char *bearer, const char *cert_serial);
   int server_http_first_user_cert_tier(const char *cert_serial, char *principal,
                                        size_t principal_cap);
   int server_http_first_user_apply_cert_grant(int mtls_authenticated, const char *cert_serial,
                                               int *tier, char *principal, size_t principal_cap);

   /* Synchronize the primary bearer with enrolled-bearer reads. Rotation clears
    * enrolled credentials; startup preserves the extras just loaded from config. */
   void server_http_update_primary_bearer(char *live, size_t live_sz, const char *bearer,
                                          int revoke_enrolled);
   void server_http_primary_bearer_snapshot(const char *live, char *out, size_t out_sz);

   /* Fixed-window per-bearer rate limiter (pure — unit-testable). State is a
    * single 60s window the caller owns. limit_per_min <= 0 disables limiting
    * (always returns 0). `now` is epoch seconds. Returns 0 when the request is
    * admitted (and records it); otherwise the number of seconds until the
    * window resets, to send as `Retry-After` with a 429. */
   typedef struct
   {
      long window_start;
      int count;
   } server_http_rate_state_t;
   int server_http_rate_check(server_http_rate_state_t *st, int limit_per_min, long now);

   /* Per-route capability matrix (pure — unit-testable, no socket).
    *
    * server_http_route_caps: the capability bitmask a /v1 route requires, equal
    *   to its NDJSON method twin (server_capability_for_method, server.h) where
    *   one exists, else a direct read/write classification. 0 = public
    *   (health/version/capabilities/models/openapi). Unrecognized (method,path)
    *   pairs return 0 so they fall through to the 404 router exactly as before
    *   (the HTTP route set is closed and explicit).
    *
    * server_http_conn_caps: the effective capability set for a connection. UDS
    *   (is_tcp==0) is same-user trusted ⇒ CAPS_ALL. On TCP an unscoped (or NULL)
    *   bearer ⇒ CAPS_AUTHENTICATED; a scope:<kind>:<id>:<secret> bearer ⇒ a
    *   query-only set (CAPS_READ_ONLY without CAP_CHAT), matching the kb-side
    *   scoped-token model: reads/queries yes, compute/inference no.
    *
    * server_http_route_allowed: 1 iff route_caps ⊆ conn_caps. This replaces the
    *   prior coarse read-vs-compute gate; a route is never loosened relative to
    *   the old behavior. */
   uint32_t server_http_route_caps(const char *method, const char *path);
   uint32_t server_http_conn_caps(int is_tcp, const char *bearer, int remote_writes);

   /* Roundtable policy mutations require an attested interactive webuser in
    * addition to their ordinary route capability. Exposed for policy tests. */
   int route_roundtable_mutation_authorized(const char *principal);
   int roundtable_policy_config_key(const char *key);

   /* Effective caps for a request after thin-client mTLS authentication. When
    * mTLS is enabled, bearer fallback is a query-only floor. A durable cert gets
    * the authenticated set at off/data and CAPS_ALL only when its verified
    * per-user write tier is full. */
   /* remote_writes.global_ignored: how many requests were refused that the
    * retired aimee.api.remote_writes would formerly have allowed. Lets an
    * operator size the cutover's impact instead of inferring it from
    * complaints. */
   uint64_t server_http_global_ignored_count(void);

   uint32_t server_http_effective_conn_caps(int is_tcp, const char *bearer, int remote_writes,
                                            int mtls_mode, int mtls_authenticated);
   int server_http_mtls_transport_allowed(int is_tcp, int mtls_mode, int mtls_authenticated,
                                          const char *method, const char *path);

   /* Dedicated P5 management transport classifier. The management leaf is
    * authorized only for the nonce/status health pair; those routes never fall
    * back to a generic roster certificate or bearer, and a management-profile
    * leaf never falls through to a generic route. TLS verification itself is
    * completed by server_tls_peer_cert before this pure classifier is called. */
   typedef enum
   {
      SERVER_HTTP_MANAGEMENT_NOT_APPLICABLE = 0,
      SERVER_HTTP_MANAGEMENT_ALLOW = 1,
      SERVER_HTTP_MANAGEMENT_DENY = 2
   } server_http_management_auth_t;
   server_http_management_auth_t server_http_management_auth(const char *method, const char *path,
                                                             int management_lane, int verified_peer,
                                                             int management_profile,
                                                             const char *peer_cn);

   /* Parse one HTTP header value (case-insensitive name) from a raw request
    * buffer into out (NUL-terminated, bounded by n). Returns 1 when found, 0
    * otherwise. Shared with the request-context populator. */
   int http_header(const char *buf, const char *name, char *out, size_t n);

   /* Populate the thread-local request context (#3) from the connection socket
    * and request headers (request id, idempotency key, transport, peer-cred
    * principal, and — only from a trusted proxy — stamped principal/source/
    * session). `caps` is the connection's effective capability set. Lives in
    * server_http_reqctx.c. */
   void server_http_populate_request_context(int fd, int is_tcp, const char *buf,
                                             const char *request_id, const char *method,
                                             const char *path, uint32_t caps);

   /* Install the authenticated surface caller after base context capture.
    * Returns -1 only when a UDS peer does not resolve to a local host account. */
   int server_http_apply_caller_context(int is_tcp, const char *request,
                                        const char *first_user_principal, int identity_present,
                                        const char *identity_subject);

   /* Resolve a kernel peer uid to the PAM/host subject forwarded to the KB.
    * Returns 0 only for a non-empty local account name. */
   int server_http_host_subject_for_uid(long uid, char *out, size_t cap);
   int server_http_route_allowed(int is_tcp, const char *bearer, const char *method,
                                 const char *path, int remote_writes);
   /* Whether a route is reachable ONLY over the local UDS listener — never over TCP,
    * whatever the bearer and whatever aimee.api.remote_writes says.
    *
    * Distinct from server_http_route_is_local_only, whose name is historical and which
    * reports whether a route dispatches a data-write op. Exposed so the property can be
    * tested directly: it is the only thing standing between a fully-trusted TCP peer and
    * grant administration, since such a peer already holds CAPS_ALL. */
   int v1_route_requires_uds(const char *method, const char *path);

   int server_http_route_allowed_caps(int is_tcp, uint32_t have, const char *method,
                                      const char *path, int remote_writes);

   /* server_http_route_is_local_only: 1 iff the (method,path) route is a
    * data-plane write. Historical name retained: at remote_writes=off these
    * routes are local-UDS-only; remote_writes=data/full can expose them over TCP
    * after the per-route capability check. Pure — unit-testable. */
   int server_http_route_is_local_only(const char *method, const char *path);

   /* Build a human-readable VS Code / OpenAI-compatible model-provider setup
    * report for the aimee.api.* loopback /v1 listener (pure — no socket, no
    * config read; the caller passes the resolved config). When http_port <= 0
    * the listener is off and the report explains how to enable it; otherwise it
    * emits the per-extension base-URL/key/model snippets pointing at /v1.
    * bearer_configured is 1 when a bearer token is set (the secret itself is
    * never included). Always NUL-terminates buf. */
   void server_http_api_status_report(int http_port, int bearer_configured, int rate_limit_per_min,
                                      char *buf, size_t n);

   /* Resolve the request id echoed as `X-Request-ID` and used in access logs
    * (pure — unit-testable). When `provided` (the request's inbound
    * X-Request-ID) is non-empty it is copied (truncated to fit); otherwise a
    * monotonic id `<pid>-<seq>` is generated. Always NUL-terminates buf. */
   void server_http_request_id(const char *provided, int pid, unsigned long seq, char *buf,
                               size_t n);

   /* Cap on concurrent SSE event streams (each holds an offloaded worker
    * thread). n <= 0 restores the built-in default (256). Call before
    * server_http_start. */
   void server_http_set_max_event_streams(int n);

   /* Stop the listener and close the socket. Safe if not started. */
   void server_http_stop(void);

   /* Hot-swap the live TCP/TLS bearer without a restart and revoke every
    * additionally-enrolled bearer. NULL/empty clears the primary. */
   void server_http_set_bearer(const char *bearer);

   /* Default HTTP socket path: <config_default_dir>/aimee-http.sock. Returns a
    * pointer to a static buffer. */
   const char *server_http_default_path(void);

   /* Route + handle one request (no socket I/O) — also called by unit tests.
    * Fills resp (cap bytes) with a JSON body and returns the HTTP status.
    * body may be NULL. */
   int server_http_route(const char *method, const char *path, const char *body, int body_len,
                         char *resp, int resp_cap);

#if AIMEE_WITH_ROUNDTABLE
   /* Submit an existing dispatch method through the HTTP async op-run machinery
    * without going through a /v1 route. `body_json` is the method body before
    * method/__run_id injection. `conn_caps` are the caller capabilities to use
    * when the worker re-dispatches the method. The helper preflights the target
    * method capability before creating a run. */
   int server_http_submit_op_run(const char *op_method, const char *body_json, uint32_t conn_caps,
                                 char *resp, int resp_cap);
#endif

   /* --- OpenAI-compatible completion seam ---
    * The /v1/chat/completions and /v1/completions routes run real inference,
    * which pulls the agent/provider dependency closure. To keep that out of
    * this translation unit (and the server_http unit test), the routes call
    * through registered handlers instead of linking the agent code directly.
    * The server registers the real handlers at startup (openai_chat_register);
    * unit tests inject a stub. Until a handler is set the routes return 503. A
    * handler parses `body`, runs the completion, writes an OpenAI-shaped JSON
    * body to resp (cap), and returns the HTTP status. */
   typedef int (*server_http_completion_fn)(const char *body, char *resp, int cap);
   void server_http_set_chat_handler(server_http_completion_fn fn);

   /* --- SSE streaming seam for POST /v1/chat/completions with "stream":true ---
    * When a stream handler is registered and the request asks to stream,
    * server_http emits an SSE response: it writes the event-stream headers,
    * the handler computes the completion and calls emit(ctx, frame) once per
    * `chat.completion.chunk` JSON frame (server_http wraps each as
    * `data: <frame>\n\n`), then server_http sends `data: [DONE]\n\n` and
    * closes. With no stream handler registered the route falls back to the
    * unary handler (which rejects streaming). */
   typedef void (*server_http_sse_emit)(void *ctx, const char *frame_json);
   typedef int (*server_http_stream_fn)(const char *body, server_http_sse_emit emit, void *ctx);
   void server_http_set_chat_stream_handler(server_http_stream_fn fn);

   /* SSE streaming seam for POST /v1/completions ("stream":true). Emits legacy
    * `text_completion` chunk frames; same envelope/[DONE] handling as the chat
    * stream. With no handler registered the route falls back to the unary
    * handler. */
   void server_http_set_completion_stream_handler(server_http_stream_fn fn);

   /* SSE streaming seam for POST /v1/responses ("stream":true). The Responses
    * API uses *typed* SSE events, so its emit carries an event name and the
    * data payload; server_http writes `event: <name>\ndata: <json>\n\n` per
    * call. Unlike chat/completions there is NO terminal `data: [DONE]` — the
    * stream ends with the handler's `response.completed` event. */
   typedef void (*server_http_sse_event_emit)(void *ctx, const char *event, const char *data_json);
   typedef int (*server_http_responses_stream_fn)(const char *body, server_http_sse_event_emit emit,
                                                  void *ctx);
   void server_http_set_responses_stream_handler(server_http_responses_stream_fn fn);
   int server_http_sse_event_format(const char *event, const char *data_json, char *buf, size_t n);
   void server_http_set_completion_handler(server_http_completion_fn fn);
   void server_http_set_embeddings_handler(server_http_completion_fn fn);
   void server_http_set_responses_handler(server_http_completion_fn fn);

   /* Anthropic Messages API ingress (POST /v1/messages) so Claude Code can drive
    * aimee's primary model. The buffered handler returns the JSON message
    * object; the stream handler emits the Anthropic typed-event sequence
    * (message_start … message_stop) via server_http_sse_event_emit, with no
    * terminal `[DONE]`. count_tokens backs POST /v1/messages/count_tokens.
    * Registered by anthropic_http_register; absent in unit tests. */
   void server_http_set_messages_handler(server_http_completion_fn fn);
   void server_http_set_messages_stream_handler(server_http_responses_stream_fn fn);
   void server_http_set_count_tokens_handler(server_http_completion_fn fn);

   /* Models-provider seam for GET /v1/models. The route always advertises the
    * local `aimee` model; a registered provider appends the configured agent
    * names (the (provider,model) bindings a client can target). It must COPY
    * each id into ids[k] (64-byte slots) — pointers into provider-internal
    * state must not escape — and return the count written (<= max). Registered
    * by openai_chat_register; absent in unit tests (aimee-only). */
#define SERVER_HTTP_MODEL_ID_MAX 64
   typedef int (*server_http_models_fn)(char ids[][SERVER_HTTP_MODEL_ID_MAX], int max);
   void server_http_set_models_provider(server_http_models_fn fn);

   /* Raw GET /v1/models provider: writes the entire JSON response body into
    * resp[cap] and returns its length (or <0 to fall back to the id-list shape
    * above). Lets a provider emit a richer schema than the OpenAI list — e.g.
    * the Codex CLI's `{models:[…]}` model-discovery shape — without pulling the
    * agent/config dependency into this unit. Registered by openai_chat_register;
    * when set it takes precedence over the id-list provider. */
   typedef int (*server_http_models_raw_fn)(char *resp, int cap);
   void server_http_set_models_raw_provider(server_http_models_raw_fn fn);

   /* Register the inference-backed chat/completions handlers (defined in
    * server/openai_chat.c). Called once during server startup. */
   void openai_chat_register(void);

   /* Register the Anthropic Messages API ingress handlers (defined in
    * server/anthropic_http.c). Called once during server startup. */
   void anthropic_http_register(void);

   /* Capture the inbound `anthropic-version` / `anthropic-beta` headers off the
    * raw request into per-request thread-locals, for the /v1/messages exact-parity
    * passthrough (forwarded upstream only when the primary speaks the Anthropic API). Called
    * once per request from the HTTP dispatch; `raw_request` is the full request
    * buffer (NULL clears). Defined in server/anthropic_http.c. */
   void anthropic_http_capture_request_headers(const char *raw_request);

   /* Retry-After (seconds) the parity passthrough relays on its own response when
    * it forwarded an upstream 429/529 that carried one (0 = none). Read by
    * send_response to emit the header. Defined in server/anthropic_http.c. */
   int anthropic_http_response_retry_after(void);

   /* --- Native /v1 REST resource seam ---
    * Some native resources are backed by subsystems (kb_client, memory, …)
    * whose dependency closure must stay out of the server_http translation
    * unit. They are exposed via JSON-provider seams: the provider returns a
    * heap-allocated JSON body (the route emits it and frees it) or NULL when
    * the backend is unavailable (the route answers 502). Until a provider is
    * registered the route answers 503. Providers are wired by
    * server_native_register() at startup (defined in server/server_api.c). */
   typedef char *(*server_http_json_provider)(void);
   /* Optional KB projection merged into GET /v1/capabilities. The provider
    * returns a heap {cli_only:[...],mcp_only:[...]} object; the route frees it. */
   void server_http_set_kb_agent_surfaces_provider(server_http_json_provider fn);
   void server_http_set_rules_provider(server_http_json_provider fn);
   /* Optional kb projection merged into GET /v1/capabilities. The provider
    * returns a heap {cli_only:[...],mcp_only:[...]} object; the route frees it. */
   void server_http_set_kb_agent_surfaces_provider(server_http_json_provider fn);

   /* GET /v1/dashboard/memory provider (arg-less JSON body, like rules). */
   void server_http_set_dashboard_memory_provider(server_http_json_provider fn);

   /* GET /v1/kb/status provider (arg-less kb/vector status JSON). */
   void server_http_set_kb_status_provider(server_http_json_provider fn);

   /* GET /v1/kb/curator provider (arg-less curator observability block). */
   void server_http_set_kb_curator_provider(server_http_json_provider fn);

   /* GET /v1/agents provider (arg-less; configured agents + default, built
    * server-side from agent config rather than proxied to aimee-kb). */
   void server_http_set_agents_provider(server_http_json_provider fn);

   /* GET /v1/roadmap provider (arg-less roadmap list JSON). */
   void server_http_set_roadmap_provider(server_http_json_provider fn);

   /* GET /v1/curiosity provider (arg-less open-curiosity-items list JSON). */
   void server_http_set_curiosity_provider(server_http_json_provider fn);

   /* GET /v1/notes provider (arg-less notes list JSON). */
   void server_http_set_notes_list_provider(server_http_json_provider fn);

   /* GET /v1/dashboard/reminders provider (arg-less reminders list JSON). */
   void server_http_set_dashboard_reminders_provider(server_http_json_provider fn);

   /* POST /v1/kb/search handler seam (parses the JSON body, runs a knowledge
    * search via kb_client, writes a JSON body to resp). Reuses the generic
    * body→(resp,status) shape; 503 until registered. Wired by
    * server_native_register. */
   void server_http_set_kb_search_handler(server_http_completion_fn fn);

   /* POST /v1/memory/recall handler seam (parses the JSON body, recalls
    * relevant memories via kb_client). Same generic body→(resp,status) shape;
    * 503 until registered. Wired by server_native_register. */
   void server_http_set_memory_recall_handler(server_http_completion_fn fn);

   /* POST /v1/notes/search handler seam (parses the JSON body, searches notes
    * via kb_client). Same generic body→(resp,status) shape; 503 until
    * registered. Wired by server_native_register. */
   void server_http_set_notes_search_handler(server_http_completion_fn fn);

   /* POST /v1/runs handler seam: runs inference on the request and stores a run
    * record (retrievable via GET /v1/runs/{id} from openai_runs_store). Same
    * generic body→(resp,status) shape; 503 until registered. Wired by
    * openai_chat_register (it pulls the agent dependency closure). */
   void server_http_set_runs_handler(server_http_completion_fn fn);

   /* GET /v1/ready readiness seam. Unlike the JSON-provider seams above, a
    * readiness answer carries a status code as well as a body, so the provider
    * mirrors the models-raw shape: write the JSON body into resp[cap] and
    * return the HTTP status (200 ready, 503 not ready).
    *
    * The provider must not perform I/O — it serves a snapshot sampled off the
    * request path — so that a slow or wedged dependency cannot stall the
    * listener. Until a provider is registered the route reports every
    * dependency `unknown` and answers 503: readiness fails closed, so an
    * unsampled server is never advertised as ready. Wired by
    * server_native_register (it pulls the dependency closure). */
   typedef int (*server_http_ready_fn)(char *resp, int cap);
   void server_http_set_ready_provider(server_http_ready_fn fn);

   /* Readiness sampler (server/server_ready.c). server_ready_register() takes
    * one sample synchronously, starts the background sampler, and registers the
    * provider above. server_ready_sample_now() forces a synchronous sample so a
    * caller never has to wait on the interval. */
   void server_ready_register(void);
   void server_ready_sample_now(void);

   /* The readiness decision as a pure function (no globals, locks, or I/O), so
    * roll-up and staleness behavior can be tested by passing a clock instead of
    * sleeping past a real interval. db1_ok/kb_ok: 1 ok, 0 fail, -1 unknown.
    * Writes the JSON body and returns the HTTP status (200 ready / 503 not). */
   typedef struct
   {
      int retrieval_ok; /* 1 usable, 0 failed, -1 unknown */
      int modules_ok;   /* required local process modules attached */
      const char *failed_boundary;
      const char *missing_module;
      const char *breaker_state;
      long long retry_after_ms;
      long long last_success_query_ms;
      const char *last_ingest_at;
   } server_ready_diagnostics_t;
   int server_ready_render(int db1_ok, int kb_ok, const server_ready_diagnostics_t *diagnostics,
                           long sampled_at, long now, int stale_secs, char *resp, int cap);

   void server_native_register(void);

   /* --- Per-session active persona (set via POST /v1/sessions/<id>/persona,
    *     read by the chat prompt builder). In-memory, bounded, thread-safe. --- */

   /* Record persona for session_id (overwrites). No-op on NULL/empty args. */
   void session_persona_set(const char *session_id, const char *persona);

   /* Look up the persona for session_id. Writes it to out (n bytes) and returns
    * 1 if set, 0 otherwise (out gets "" ). */
   int session_persona_get(const char *session_id, char *out, size_t n);

   /* Durable first-ingress delivery reservation, tied to the server session row
    * and therefore removed by normal session expiration. claim returns 1 for a
    * new reservation, 0 when already delivered/in flight, and -1 on storage
    * failure. finish commits or releases it. */
   int session_persona_delivery_claim(const char *session_id);
   void session_persona_delivery_finish(const char *session_id, int delivered);

   /* --- Per-session active primary agent (set via POST
    *     /v1/sessions/<id>/primary, used as the chat fallback agent when a
    *     request names no explicit provider). In-memory, bounded, thread-safe. */

   /* Record the active primary agent name for session_id (overwrites).
    * No-op on NULL/empty args. */
   void session_primary_set(const char *session_id, const char *agent);

   /* Look up the active primary agent for session_id. Writes it to out (n
    * bytes) and returns 1 if set, 0 otherwise (out gets ""). */
   int session_primary_get(const char *session_id, char *out, size_t n);

   /* Clear any pinned primary for session_id (the session reverts to the
    * default provider). No-op on NULL/empty/unknown session. */
   void session_primary_clear(const char *session_id);

   /* Delegation admission check: reject an empty/too-short prompt, then enforce
    * the active persona's delegate policy (session's persona, else the durable
    * default — none, or readonly + a write `role`). On rejection, write the
    * reason to buf (n) and return it; otherwise return NULL. */
   const char *server_http_delegate_block(const char *session_id, const char *role,
                                          const char *prompt, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_HTTP_H */
