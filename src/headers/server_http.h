/* server_http.h: aimee-server inbound HTTP-over-UDS /v1 API.
 *
 * First slice of the aimee-server-http-api proposal: a small hand-rolled
 * HTTP/1.1 listener bound to a dedicated Unix socket, serving the /v1/personas
 * resource. New handlers write HTTP responses directly (no conn-transport
 * virtualization / OpenAI shaping — those are later phases). Mirrors the
 * aimee-kb HTTP server (src/kb/http/). */
#ifndef DEC_SERVER_HTTP_H
#define DEC_SERVER_HTTP_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Start the HTTP listener(s), spawning a detached accept-loop thread that
    * polls all bound sockets. The UDS at uds_path (unlinked first;
    * filesystem-permission auth, no token) is always bound. When tcp_port > 0
    * AND bearer_token is non-empty, a localhost (127.0.0.1) TCP listener is
    * also bound and gated by `Authorization: Bearer <bearer_token>`; a
    * configured port with no bearer is refused (UDS still binds). Returns 0 if
    * the UDS bound, -1 on error. Idempotent-safe: a second call while running
    * returns -1. rate_limit_per_min caps authorized TCP requests per 60s
    * window (0 = unlimited); over-limit ⇒ 429 + Retry-After. */
   int server_http_start(const char *uds_path, int tcp_port, const char *bearer_token,
                         int rate_limit_per_min, int remote_writes);

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
   int server_http_route_allowed(int is_tcp, const char *bearer, const char *method,
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

   /* Default HTTP socket path: <config_default_dir>/aimee-http.sock. Returns a
    * pointer to a static buffer. */
   const char *server_http_default_path(void);

   /* Route + handle one request (no socket I/O) — also called by unit tests.
    * Fills resp (cap bytes) with a JSON body and returns the HTTP status.
    * body may be NULL. */
   int server_http_route(const char *method, const char *path, const char *body, int body_len,
                         char *resp, int resp_cap);

   /* Submit an existing dispatch method through the HTTP async op-run machinery
    * without going through a /v1 route. `body_json` is the method body before
    * method/__run_id injection. `conn_caps` are the caller capabilities to use
    * when the worker re-dispatches the method. The helper preflights the target
    * method capability before creating a run. */
   int server_http_submit_op_run(const char *op_method, const char *body_json, uint32_t conn_caps,
                                 char *resp, int resp_cap);

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

   /* --- Native /v1 REST resource seam ---
    * Some native resources are backed by subsystems (kb_client, memory, …)
    * whose dependency closure must stay out of the server_http translation
    * unit. They are exposed via JSON-provider seams: the provider returns a
    * heap-allocated JSON body (the route emits it and frees it) or NULL when
    * the backend is unavailable (the route answers 502). Until a provider is
    * registered the route answers 503. Providers are wired by
    * server_native_register() at startup (defined in server/server_api.c). */
   typedef char *(*server_http_json_provider)(void);
   void server_http_set_rules_provider(server_http_json_provider fn);

   /* GET /v1/dashboard/memory provider (arg-less JSON body, like rules). */
   void server_http_set_dashboard_memory_provider(server_http_json_provider fn);

   /* GET /v1/kb/status provider (arg-less kb/vector status JSON). */
   void server_http_set_kb_status_provider(server_http_json_provider fn);

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

   void server_native_register(void);

   /* --- Per-session active persona (set via POST /v1/sessions/<id>/persona,
    *     read by the chat prompt builder). In-memory, bounded, thread-safe. --- */

   /* Record persona for session_id (overwrites). No-op on NULL/empty args. */
   void session_persona_set(const char *session_id, const char *persona);

   /* Look up the persona for session_id. Writes it to out (n bytes) and returns
    * 1 if set, 0 otherwise (out gets "" ). */
   int session_persona_get(const char *session_id, char *out, size_t n);

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
