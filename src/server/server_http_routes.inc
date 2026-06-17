/* server_http_routes.inc — the declarative /v1 route registry.
 *
 * Included into server_http.c (single translation unit) after all the route_*
 * helper functions and g_* handler-seam globals are defined. This table is the
 * single source of truth for the /v1 surface: it drives request dispatch
 * (v1_route_dispatch / server_http_route), the per-route capability gate
 * (v1_route_caps_lookup / server_http_route_caps), and — via its literal path
 * strings, which the server-api conformance check scans — the OpenAPI path
 * inventory. Adding a route is one row here plus a one-line adapter; the two
 * code paths can no longer drift, which is the whole point of WP0.2 in the
 * /v1 hub-migration plan.
 *
 * Streaming routes (/v1/chat/stream and the /events sub-resources) are
 * dispatched directly by handle_conn before the buffered router is reached;
 * their rows carry handler == NULL so they still participate in the capability
 * gate and the conformance scan without being dispatched here. */

/* Per-request context handed to a route handler. `id` holds the extracted
 * dynamic path segment (persona / role-template name, session id, run id) for
 * RM_PREFIX routes, or "" for fixed routes. */
typedef struct
{
   const char *method;
   const char *path;
   const char *body;
   int body_len;
   const char *id;
   const char *op; /* matched row's NDJSON method twin (for rh_dispatch_op), or NULL */
} route_req_t;

typedef int (*route_handler_fn)(const route_req_t *rq, char *resp, int cap);

typedef enum
{
   RM_EXACT,  /* path == entry->path */
   RM_PREFIX, /* path == entry->path + <id> ( + entry->suffix ), <id> one segment */
} route_match_kind_t;

/* One row of the /v1 route registry. `op`, when non-NULL, derives the required
 * capability from the NDJSON method twin (server_capability_for_method) so the
 * HTTP route inherits exactly its socket-method capability; otherwise `caps` is
 * used verbatim. `handler` is NULL for streaming routes (see file header). */
typedef struct
{
   const char *verb;   /* "GET" / "POST" / "PUT" / "DELETE" */
   const char *path;   /* exact path, or static prefix for RM_PREFIX */
   const char *suffix; /* trailing segment for RM_PREFIX (e.g. "/stop"), else NULL */
   route_match_kind_t kind;
   const char *op;           /* NDJSON method twin for cap derivation, or NULL */
   uint32_t caps;            /* required caps when op == NULL */
   route_handler_fn handler; /* buffered handler, or NULL for streaming routes */
} http_route_t;

/* ── route handler adapters ───────────────────────────────────────────────
 * Thin uniform-signature wrappers over the existing route_* helpers so a single
 * function-pointer table can dispatch them all. */

static int rh_health(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_health(resp, cap);
}
static int rh_version(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_version(resp, cap);
}
static int rh_capabilities(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_capabilities(resp, cap);
}
static int rh_models(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_models(resp, cap);
}
static int rh_openapi(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   snprintf(resp, (size_t)cap, "%s", AIMEE_OPENAPI_SERVER_YAML_STR);
   return 200;
}

static int rh_rules(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_rules_provider, resp, cap, "rules");
}
static int rh_dashboard_memory(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_dashboard_memory_provider, resp, cap, "dashboard");
}
static int rh_dashboard_reminders(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_dashboard_reminders_provider, resp, cap, "dashboard");
}
static int rh_kb_status(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_kb_status_provider, resp, cap, "kb status");
}
/* Curator observability provider (§4). Kept here rather than in server_http.c
 * (which is at its line-count limit); routes.inc is part of the same TU. */
static server_http_json_provider g_kb_curator_provider = NULL;
void server_http_set_kb_curator_provider(server_http_json_provider fn)
{
   g_kb_curator_provider = fn;
}
static int rh_kb_curator(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_kb_curator_provider, resp, cap, "kb curator");
}
static int rh_agents(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_agents_provider, resp, cap, "agents");
}
static int rh_roadmap(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_roadmap_provider, resp, cap, "roadmap");
}
static int rh_curiosity(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_curiosity_provider, resp, cap, "curiosity");
}
static int rh_notes(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_json_provider(g_notes_list_provider, resp, cap, "notes");
}

static int rh_kb_search(const route_req_t *rq, char *resp, int cap)
{
   return route_native_post(g_kb_search_handler, rq->body, resp, cap,
                            "knowledge search is not available on this server");
}
static int rh_memory_recall(const route_req_t *rq, char *resp, int cap)
{
   return route_native_post(g_memory_recall_handler, rq->body, resp, cap,
                            "memory recall is not available on this server");
}
static int rh_notes_search(const route_req_t *rq, char *resp, int cap)
{
   return route_native_post(g_notes_search_handler, rq->body, resp, cap,
                            "notes search is not available on this server");
}
static int rh_runs_post(const route_req_t *rq, char *resp, int cap)
{
   return route_native_post(g_runs_handler, rq->body, resp, cap,
                            "runs are not available on this server");
}

static int rh_chat(const route_req_t *rq, char *resp, int cap)
{
   return route_completion(g_chat_handler, rq->body, resp, cap);
}
static int rh_completions(const route_req_t *rq, char *resp, int cap)
{
   return route_completion(g_completion_handler, rq->body, resp, cap);
}
static int rh_embeddings(const route_req_t *rq, char *resp, int cap)
{
   return route_completion(g_embeddings_handler, rq->body, resp, cap);
}
static int rh_responses(const route_req_t *rq, char *resp, int cap)
{
   return route_completion(g_responses_handler, rq->body, resp, cap);
}
/* POST /v1/messages buffered (stream:false). The stream:true case is intercepted
 * in handle_conn before the buffered router (see g_messages_stream_handler). */
static int rh_messages(const route_req_t *rq, char *resp, int cap)
{
   return route_completion(g_messages_handler, rq->body, resp, cap);
}
static int rh_count_tokens(const route_req_t *rq, char *resp, int cap)
{
   return route_completion(g_count_tokens_handler, rq->body, resp, cap);
}

/* ── server-hosted Claude PTY session (terminal forwarding) ────────────────
 * POST /v1/cli/session            — create/ensure a server-hosted claude session
 * POST /v1/cli/session/<id>/input — queue raw input (base64) to the PTY
 * POST /v1/cli/session/<id>/resize— set the PTY window size
 * DELETE /v1/cli/session/<id>     — kill the session
 * GET  /v1/cli/session/<id>/stream— SSE of raw PTY output (handle_conn, no handler)
 * The cli_cmd is fixed server-side to "claude" (scope: Claude only) so a client
 * cannot turn this into arbitrary command execution. */
static int rh_cli_session_create(const route_req_t *rq, char *resp, int cap)
{
   if (!cli_session_pty_forwarding_enabled())
      return err_json(resp, cap, 404, "cli session forwarding disabled");
   cJSON *req = rq->body ? cJSON_Parse(rq->body) : NULL;
   if (!req)
      return err_json(resp, cap, 400, "invalid JSON body");
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *id = cJSON_IsString(jid) ? jid->valuestring : NULL;
   if (!id || !id[0] || !is_safe_id(id))
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "valid session_id required");
   }
   cJSON *jrows = cJSON_GetObjectItemCaseSensitive(req, "rows");
   cJSON *jcols = cJSON_GetObjectItemCaseSensitive(req, "cols");
   int rows = cJSON_IsNumber(jrows) ? jrows->valueint : 0;
   int cols = cJSON_IsNumber(jcols) ? jcols->valueint : 0;
   char err[160] = "";
   int rc = cli_session_pty_ensure(id, "claude", NULL, rows, cols, err, sizeof(err));
   cJSON_Delete(req);
   if (rc != 0)
      return err_json(resp, cap, 500, err[0] ? err : "failed to create session");
   snprintf(resp, (size_t)cap, "{\"status\":\"ok\"}");
   return 200;
}
static int rh_cli_session_input(const route_req_t *rq, char *resp, int cap)
{
   if (!cli_session_pty_forwarding_enabled())
      return err_json(resp, cap, 404, "cli session forwarding disabled");
   cJSON *req = rq->body ? cJSON_Parse(rq->body) : NULL;
   if (!req)
      return err_json(resp, cap, 400, "invalid JSON body");
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(req, "data");
   if (!cJSON_IsString(jd))
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "base64 \"data\" required");
   }
   size_t blen = strlen(jd->valuestring);
   unsigned char *buf = malloc(blen + 1);
   if (!buf)
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 500, "oom");
   }
   size_t n = aimee_base64_decode(jd->valuestring, buf, blen + 1);
   int rc = (n == (size_t)-1) ? -1 : cli_session_pty_input(rq->id, buf, n);
   free(buf);
   cJSON_Delete(req);
   if (rc != 0)
      return err_json(resp, cap, 404, "unknown session or invalid input");
   snprintf(resp, (size_t)cap, "{\"status\":\"ok\"}");
   return 200;
}
static int rh_cli_session_resize(const route_req_t *rq, char *resp, int cap)
{
   if (!cli_session_pty_forwarding_enabled())
      return err_json(resp, cap, 404, "cli session forwarding disabled");
   cJSON *req = rq->body ? cJSON_Parse(rq->body) : NULL;
   if (!req)
      return err_json(resp, cap, 400, "invalid JSON body");
   cJSON *jr = cJSON_GetObjectItemCaseSensitive(req, "rows");
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "cols");
   int rows = cJSON_IsNumber(jr) ? jr->valueint : 0;
   int cols = cJSON_IsNumber(jc) ? jc->valueint : 0;
   int rc = cli_session_pty_resize(rq->id, rows, cols);
   cJSON_Delete(req);
   if (rc != 0)
      return err_json(resp, cap, 404, "unknown session or bad dimensions");
   snprintf(resp, (size_t)cap, "{\"status\":\"ok\"}");
   return 200;
}
static int rh_cli_session_delete(const route_req_t *rq, char *resp, int cap)
{
   if (!cli_session_pty_forwarding_enabled())
      return err_json(resp, cap, 404, "cli session forwarding disabled");
   cli_session_pty_kill(rq->id);
   snprintf(resp, (size_t)cap, "{\"status\":\"ok\"}");
   return 200;
}

static int rh_runs_get(const route_req_t *rq, char *resp, int cap)
{
   return route_runs_get(rq->id, resp, cap);
}
static int rh_runs_stop(const route_req_t *rq, char *resp, int cap)
{
   return route_runs_stop(rq->id, resp, cap);
}

static int rh_persona_current(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_persona_current(resp, cap);
}
static int rh_personas_list(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_personas_list(resp, cap);
}
static int rh_personas_create(const route_req_t *rq, char *resp, int cap)
{
   return route_persona_upsert(NULL, rq->body, resp, cap);
}
static int rh_persona_show(const route_req_t *rq, char *resp, int cap)
{
   return route_persona_show(rq->id, resp, cap);
}
static int rh_persona_put(const route_req_t *rq, char *resp, int cap)
{
   return route_persona_upsert(rq->id, rq->body, resp, cap);
}
static int rh_persona_delete(const route_req_t *rq, char *resp, int cap)
{
   return route_persona_remove(rq->id, resp, cap);
}

static int rh_role_templates_list(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_role_templates_list(resp, cap);
}
static int rh_role_template_show(const route_req_t *rq, char *resp, int cap)
{
   return route_role_template_show(rq->id, resp, cap);
}
static int rh_role_template_put(const route_req_t *rq, char *resp, int cap)
{
   return route_role_template_upsert(rq->id, rq->body, resp, cap);
}
static int rh_role_template_delete(const route_req_t *rq, char *resp, int cap)
{
   return route_role_template_remove(rq->id, resp, cap);
}

static int rh_sessions_list(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_sessions_list(resp, cap);
}
static int rh_session_attach(const route_req_t *rq, char *resp, int cap)
{
   return route_session_attach(rq->id, rq->body, resp, cap);
}
static int rh_session_detach(const route_req_t *rq, char *resp, int cap)
{
   return route_session_detach(rq->id, rq->body, resp, cap);
}
static int rh_session_persona_get(const route_req_t *rq, char *resp, int cap)
{
   return route_session_persona_get(rq->id, resp, cap);
}
static int rh_session_persona_set(const route_req_t *rq, char *resp, int cap)
{
   return route_session_persona_set(rq->id, rq->body, resp, cap);
}
static int rh_session_primary_get(const route_req_t *rq, char *resp, int cap)
{
   return route_session_primary_get(rq->id, resp, cap);
}
static int rh_session_primary_set(const route_req_t *rq, char *resp, int cap)
{
   return route_session_primary_set(rq->id, rq->body, resp, cap);
}
static int rh_session_primary_clear(const route_req_t *rq, char *resp, int cap)
{
   return route_session_primary_clear(rq->id, resp, cap);
}

/* ── workflow visual composer (W7) ────────────────────────────────────────
 * Read+author surface over the wfe_ definition model + DB1 run-state; logic in
 * server_workflow_api.c (self-contained JSON envelopes). */
static int rh_wf_blocks(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return wf_api_blocks(resp, cap);
}
static int rh_wf_list(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return wf_api_list(resp, cap);
}
static int rh_wf_get(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_get(rq->id, resp, cap);
}
static int rh_wf_validate(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_validate(rq->body, resp, cap);
}
static int rh_wf_save(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_save(rq->body, resp, cap);
}
static int rh_wf_items(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return wf_api_items(resp, cap);
}
static int rh_wf_item(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_item(rq->id, resp, cap);
}

/* The POST /v1/rpc bridge was retired once every dispatch method gained a
 * first-class /v1 route (op-parity complete; check-v1-method-coverage reports 0
 * excluded). The thin client and all co-located callers now use the dedicated
 * routes via rh_dispatch_op below; loopback_rpc remains the shared dispatch seam. */

/* Generic adapter for routes backed directly by an NDJSON method via the
 * server_dispatch loopback bridge: it injects the row's `op` as the request
 * "method" into a copy of the request body and runs it. This is the P1
 * op-parity mechanism — a new REST family is a table row (verb + path + op +
 * rh_dispatch_op), with no bespoke handler. Capability is the row's op-derived
 * cap, enforced by the outer route gate AND re-checked per method by
 * server_dispatch against the connection caps (g_rpc_conn_caps). Only safe for
 * single-response methods (the buffered HTTP listener must not block): streaming
 * or foreground-blocking methods must not use this. */
static int rh_dispatch_op(const route_req_t *rq, char *resp, int cap)
{
   if (!rq->op || !rq->op[0])
      return err_json(resp, cap, 500, "route has no dispatch method");
   cJSON *req = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : cJSON_CreateObject();
   if (!req || !cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   /* method is server-set from the matched row, never the client body. */
   cJSON_DeleteItemFromObjectCaseSensitive(req, "method");
   cJSON_AddStringToObject(req, "method", rq->op);
   char *line = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!line)
      return err_json(resp, cap, 500, "out of memory");
   int rc = loopback_rpc(line, (int)strlen(line), resp, cap, g_rpc_conn_caps);
   free(line);
   return rc;
}

/* ── async op runner ──────────────────────────────────────────────────────────
 * Long-running / LLM methods (kb.build, graph.sync_code, memory.benchmark,
 * curator.synthesize, …) cannot use rh_dispatch_op: the inline bridge would run
 * them on the single listener thread and stall all /v1 serving. rh_dispatch_op_async
 * instead enqueues the dispatch onto a detached worker, returns a `queued` run
 * handle immediately, and the worker drives the loopback RPC to completion and
 * finalizes the record. Status/result are polled via GET /v1/runs/{id} (the run
 * store is generic — it holds whatever JSON snapshot we store). Capability is the
 * row's op-derived cap, gated before we enqueue. */
typedef struct
{
   char run_id[64];
   char op[64];
   char *line;         /* dispatch body with method injected; owned by worker */
   uint32_t conn_caps; /* caps captured at enqueue time */
   long created;
} op_run_job_t;

/* Build the run snapshot object stored for GET /v1/runs/{id}. `result` is an
 * already-serialized JSON value (object/array/string) or NULL while queued. */
static char *op_run_snapshot(const char *run_id, const char *op, const char *status, long created,
                             const char *result)
{
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return NULL;
   cJSON_AddStringToObject(o, "id", run_id);
   cJSON_AddStringToObject(o, "object", "op.run");
   cJSON_AddStringToObject(o, "method", op);
   cJSON_AddStringToObject(o, "status", status);
   cJSON_AddNumberToObject(o, "created", (double)created);
   if (result)
   {
      cJSON *r = cJSON_Parse(result);
      cJSON_AddItemToObject(o, "result", r ? r : cJSON_CreateString(result));
   }
   char *s = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   return s;
}

static void op_run_worker_run(void *arg)
{
   op_run_job_t *j = (op_run_job_t *)arg;
   compute_pool_set_job(POOL_JOB_DELEGATE, "op=%s run=%s", j->op, j->run_id);
   openai_runs_store_set_status(j->run_id, OPENAI_RUN_IN_PROGRESS);
   char *started = op_run_snapshot(j->run_id, j->op, "in_progress", j->created, NULL);
   if (started)
   {
      openai_runs_store_update_json(j->run_id, started);
      free(started);
   }

   char *buf = (char *)malloc(SHTTP_RESP_MAX);
   if (!buf)
   {
      char *snap =
          op_run_snapshot(j->run_id, j->op, "failed", j->created, "{\"error\":\"out of memory\"}");
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED, snap ? snap : "{}");
      free(snap);
      free(j->line);
      free(j);
      compute_pool_clear_job();
      return;
   }

   int rc = loopback_rpc(j->line, (int)strlen(j->line), buf, SHTTP_RESP_MAX, j->conn_caps);
   /* A dispatch returning a JSON object with "error" (or a non-2xx rc) is a
    * failed run; anything else is the completed result payload. */
   int ok = (rc >= 200 && rc < 300);
   openai_run_status_t terminal_status = OPENAI_RUN_COMPLETED;
   if (ok)
   {
      cJSON *parsed = cJSON_Parse(buf);
      if (parsed)
      {
         if (cJSON_GetObjectItemCaseSensitive(parsed, "error"))
            ok = 0;
         cJSON *cancelled = cJSON_GetObjectItemCaseSensitive(parsed, "cancelled");
         if (cJSON_IsTrue(cancelled))
            terminal_status = OPENAI_RUN_CANCELLED;
         cJSON_Delete(parsed);
      }
   }
   const char *status_str =
       terminal_status == OPENAI_RUN_CANCELLED ? "cancelled" : (ok ? "completed" : "failed");
   char *snap = op_run_snapshot(j->run_id, j->op, status_str, j->created, buf);
   openai_runs_store_finalize(j->run_id, ok ? terminal_status : OPENAI_RUN_FAILED,
                              snap ? snap : buf);
   /* Pipeline-owned result capture (#18): persist the terminal attempt into the
    * durable DB1 ledger before the bounded /v1/runs record can be evicted. This
    * no-ops for ordinary (non-pipeline) op-runs. */
   rtp_seam_finalize(j->run_id, ok, terminal_status == OPENAI_RUN_CANCELLED, buf);
   free(snap);
   free(buf);
   free(j->line);
   free(j);
   compute_pool_clear_job();
}

static void *op_run_worker_thread(void *arg)
{
   op_run_worker_run(arg);
   return NULL;
}

static int submit_op_run_internal(const char *op_method, const char *body_json, uint32_t conn_caps,
                                  int preflight_caps, char *resp, int cap)
{
   if (!op_method || !op_method[0])
      return err_json(resp, cap, 500, "route has no dispatch method");
   uint32_t required = server_capability_for_method(op_method);
   if (preflight_caps && required && (conn_caps & required) == 0)
      return err_json(resp, cap, 403, "forbidden: insufficient capabilities");
   cJSON *req = (body_json && body_json[0]) ? cJSON_Parse(body_json) : cJSON_CreateObject();
   if (!req || !cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   cJSON_DeleteItemFromObjectCaseSensitive(req, "method");
   cJSON_AddStringToObject(req, "method", op_method);

   long created = (long)time(NULL);
   static atomic_ulong g_op_run_seq = 0;
   char id[64];
   unsigned long seq = atomic_fetch_add_explicit(&g_op_run_seq, 1, memory_order_relaxed) + 1;
   snprintf(id, sizeof(id), "oprun_%ld_%lu", created, seq);

   cJSON_DeleteItemFromObjectCaseSensitive(req, "__run_id");
   cJSON_AddStringToObject(req, "__run_id", id);

   /* Pipeline-owned submission seam (#20/#21): a validated `pipeline_pass_id`
    * registers an attempt row keyed to this run id and is forwarded internally
    * as the private `__pipeline_pass_id`. A caller-supplied double-underscore id
    * is never honored. An unknown / non-owned pass id is rejected. */
   cJSON_DeleteItemFromObjectCaseSensitive(req, "__pipeline_pass_id");
   cJSON *ppid = cJSON_GetObjectItemCaseSensitive(req, "pipeline_pass_id");
   if (ppid && cJSON_IsNumber(ppid))
   {
      int pass_id = (int)ppid->valuedouble;
      cJSON_DeleteItemFromObjectCaseSensitive(req, "pipeline_pass_id");
      if (rtp_seam_register_attempt(pass_id, id) < 0)
      {
         cJSON_Delete(req);
         return err_json(resp, cap, 403, "invalid or non-owned pipeline_pass_id");
      }
      cJSON_AddNumberToObject(req, "__pipeline_pass_id", pass_id);
   }

   char *line = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!line)
      return err_json(resp, cap, 500, "out of memory");

   char *queued = op_run_snapshot(id, op_method, "queued", created, NULL);
   if (!queued || !openai_runs_store_create(id, queued))
   {
      free(queued);
      free(line);
      return err_json(resp, cap, 500, "could not create run");
   }

   op_run_job_t *j = (op_run_job_t *)calloc(1, sizeof(*j));
   if (!j)
   {
      free(queued);
      free(line);
      return err_json(resp, cap, 500, "out of memory");
   }
   snprintf(j->run_id, sizeof(j->run_id), "%s", id);
   snprintf(j->op, sizeof(j->op), "%s", op_method);
   j->line = line; /* ownership moves to the worker */
   j->conn_caps = conn_caps;
   j->created = created;

   server_ctx_t *ctx = server_active_ctx();
   if (ctx)
   {
      if (compute_pool_submit(&ctx->pool, op_run_worker_run, j) != 0)
      {
         char *failed = op_run_snapshot(id, op_method, "failed", created,
                                        "{\"error\":\"compute queue full\"}");
         openai_runs_store_finalize(id, OPENAI_RUN_FAILED, failed ? failed : queued);
         free(failed);
         free(j->line);
         free(j);
         free(queued);
         return err_json(resp, cap, 503, "compute queue full");
      }
   }
   else
   {
      pthread_t th;
      if (pthread_create(&th, NULL, op_run_worker_thread, j) != 0)
      {
         char *failed = op_run_snapshot(id, op_method, "failed", created,
                                        "{\"error\":\"could not start run\"}");
         openai_runs_store_finalize(id, OPENAI_RUN_FAILED, failed ? failed : queued);
         free(failed);
         free(j->line);
         free(j);
         free(queued);
         return err_json(resp, cap, 500, "could not start run");
      }
      pthread_detach(th);
   }

   int n = snprintf(resp, (size_t)cap, "%s", queued);
   free(queued);
   return (n > 0 && n < cap) ? 200 : 200;
}

int server_http_submit_op_run(const char *op_method, const char *body_json, uint32_t conn_caps,
                              char *resp, int cap)
{
   return submit_op_run_internal(op_method, body_json, conn_caps, 1, resp, cap);
}

static int rh_dispatch_op_async(const route_req_t *rq, char *resp, int cap)
{
   return submit_op_run_internal(rq->op, rq->body, g_rpc_conn_caps, 0, resp, cap);
}

/* ── workspace resource plane (workspace-resource-plane §1) ───────────────────
 * The instance-scoped workspace registry over /v1. The backing RPCs
 * (workspace.add/get/list/remove) take a positional `args` array, so register
 * (REST body) and the {id}-bearing get/remove need bespoke shaping rather than
 * rh_dispatch_op. The {id} segment is the workspace's absolute path, carried
 * percent-encoded so its '/'s survive as one path segment. */

static int ws_hex(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Percent-decode `in` into `out` (cap bytes incl. NUL); a malformed %XX is
 * copied literally. Returns out. */
static char *ws_pct_decode(const char *in, char *out, size_t cap)
{
   size_t o = 0;
   for (size_t i = 0; in && in[i] && o + 1 < cap; i++)
   {
      int hi, lo;
      if (in[i] == '%' && (hi = ws_hex(in[i + 1])) >= 0 && (lo = ws_hex(in[i + 2])) >= 0)
      {
         out[o++] = (char)((hi << 4) | lo);
         i += 2;
      }
      else
      {
         out[o++] = in[i];
      }
   }
   if (cap)
      out[o] = '\0';
   return out;
}

/* Build {"method":m,"args":[arg0, extra...]} and run it through the loopback
 * bridge (same path + conn caps as rh_dispatch_op). */
static int ws_dispatch_args(const char *method, const char *arg0, const char *const *extra,
                            int extra_n, char *resp, int cap)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return err_json(resp, cap, 500, "out of memory");
   cJSON_AddStringToObject(req, "method", method);
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   if (arg0)
      cJSON_AddItemToArray(args, cJSON_CreateString(arg0));
   for (int i = 0; i < extra_n; i++)
      cJSON_AddItemToArray(args, cJSON_CreateString(extra[i]));
   char *line = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!line)
      return err_json(resp, cap, 500, "out of memory");
   int rc = loopback_rpc(line, (int)strlen(line), resp, cap, g_rpc_conn_caps);
   free(line);
   return rc;
}

/* POST /v1/workspaces — register {root_hint|root|path, provider?}. */
static int rh_workspaces_register(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : cJSON_CreateObject();
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jroot = cJSON_GetObjectItemCaseSensitive(body, "root_hint");
   if (!cJSON_IsString(jroot))
      jroot = cJSON_GetObjectItemCaseSensitive(body, "root");
   if (!cJSON_IsString(jroot))
      jroot = cJSON_GetObjectItemCaseSensitive(body, "path");
   const char *root = (cJSON_IsString(jroot) && jroot->valuestring) ? jroot->valuestring : "";
   const cJSON *jprov = cJSON_GetObjectItemCaseSensitive(body, "provider");
   const char *provider = (cJSON_IsString(jprov) && jprov->valuestring) ? jprov->valuestring : "";
   int rc;
   if (!root[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "missing root_hint");
   }
   if (provider[0])
   {
      const char *extra[2] = {"--provider", provider};
      rc = ws_dispatch_args("workspace.add", root, extra, 2, resp, cap);
   }
   else
   {
      rc = ws_dispatch_args("workspace.add", root, NULL, 0, resp, cap);
   }
   cJSON_Delete(body);
   return rc;
}

/* GET /v1/workspaces/{id} — manifest for the percent-encoded path id. */
static int rh_workspace_get(const route_req_t *rq, char *resp, int cap)
{
   char path[MAX_PATH_LEN];
   ws_pct_decode(rq->id, path, sizeof(path));
   if (!path[0])
      return err_json(resp, cap, 400, "missing workspace id");
   return ws_dispatch_args("workspace.get", path, NULL, 0, resp, cap);
}

/* DELETE /v1/workspaces/{id} — deregister the percent-encoded path id. */
static int rh_workspace_remove(const route_req_t *rq, char *resp, int cap)
{
   char path[MAX_PATH_LEN];
   ws_pct_decode(rq->id, path, sizeof(path));
   if (!path[0])
      return err_json(resp, cap, 400, "missing workspace id");
   return ws_dispatch_args("workspace.remove", path, NULL, 0, resp, cap);
}

/* POST /v1/workspaces/{id}/forge-token — install a short-lived brokered forge
 * token for the workspace (workspace-resource-plane §4). Body:
 * {token, scope?, ttl_seconds?}. The token is held in memory only and injected
 * into the server's git/gh exec env; it is never written to disk or logged, and
 * is revoked when the workspace closes. tool:execute (write); reachable over TCP
 * so a filesystem-rich client hands its token to a detached/co-located hub. */
static int rh_workspace_forge_token(const route_req_t *rq, char *resp, int cap)
{
   char path[MAX_PATH_LEN];
   ws_pct_decode(rq->id, path, sizeof(path));
   if (!path[0])
      return err_json(resp, cap, 400, "missing workspace id");
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jtok = cJSON_GetObjectItemCaseSensitive(body, "token");
   const cJSON *jscope = cJSON_GetObjectItemCaseSensitive(body, "scope");
   const cJSON *jttl = cJSON_GetObjectItemCaseSensitive(body, "ttl_seconds");
   const char *token = (cJSON_IsString(jtok) && jtok->valuestring) ? jtok->valuestring : NULL;
   const char *scope =
       (cJSON_IsString(jscope) && jscope->valuestring) ? jscope->valuestring : "project";
   long ttl = cJSON_IsNumber(jttl) ? (long)jttl->valuedouble : 3600;
   if (!token || !token[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "missing token");
   }
   int rc = forge_cred_install(path, token, scope, ttl, (long)time(NULL));
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 400, "invalid token / scope / ttl, or registry full");
   snprintf(resp, (size_t)cap, "{\"ok\":true}");
   return 200;
}

/* POST /v1/workspace/clone {url, name?} — clone a repo as a project under the
 * calling webchat user's scoped workspace (webchat-git WP-D). The caller
 * principal comes from the attested identity (server.token-gated X-Aimee-Webuser),
 * NOT the body — a user can only clone into their own tree. Credentials are
 * injected from the user's sealed vault (WP-C); never accepted in the body. */
static int rh_workspace_clone(const route_req_t *rq, char *resp, int cap)
{
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(body, "url");
   const cJSON *jname = cJSON_GetObjectItemCaseSensitive(body, "name");
   const char *url = (cJSON_IsString(jurl) && jurl->valuestring) ? jurl->valuestring : NULL;
   const char *name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : NULL;

   char dest[MAX_PATH_LEN], pname[128], err[256];
   int rc = git_project_clone(principal, url, name, dest, sizeof(dest), pname, sizeof(pname), err,
                              sizeof(err));
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 400, err);

   /* Best-effort index so the new project is searchable by the agent + listed. */
   index_scan_project(pname, dest, 0);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "name", pname);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* ── detached-runner reverse channel over /v1 (workspace-resource-plane §3) ──
 * The filesystem-authority client serving a `detached` workspace polls for the
 * next op the server needs done against its working tree, executes it locally,
 * and posts the result back. These are the TCP-reachable twins of the
 * local-socket `runner.poll` / `runner.respond` RPCs — gated by tool:execute
 * (the caller IS the fs/exec authority), so a remote serving client can drive
 * them over the authenticated /v1 listener. The HTTP poll uses a SHORT bounded
 * wait (the single-threaded listener must not block on the 25s socket poll);
 * the client simply re-polls, exactly as its serve loop already does. */
#define V1_RUNNER_POLL_MS 2000

static const char *rq_body_str(const cJSON *body, const char *key)
{
   const cJSON *v = body ? cJSON_GetObjectItemCaseSensitive(body, key) : NULL;
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

/* POST /v1/runner/poll {workspace_id} → {ok, have_op, op?} */
static int rh_runner_poll(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   const char *wsid = rq_body_str(body, "workspace_id");
   if (!wsid || !wsid[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "missing workspace_id");
   }
   cJSON *op = ws_runner_registry_poll(wsid, V1_RUNNER_POLL_MS);
   cJSON_Delete(body);

   cJSON *out = cJSON_CreateObject();
   if (!out)
   {
      cJSON_Delete(op);
      return err_json(resp, cap, 500, "out of memory");
   }
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddBoolToObject(out, "have_op", op != NULL);
   if (op)
      cJSON_AddItemToObject(out, "op", op); /* transfers ownership */
   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   if (!s)
      return err_json(resp, cap, 500, "out of memory");
   snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return 200;
}

/* POST /v1/runner/respond {workspace_id, response:{...}} → {ok} */
static int rh_runner_respond(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   const char *wsid = rq_body_str(body, "workspace_id");
   const cJSON *r = body ? cJSON_GetObjectItemCaseSensitive(body, "response") : NULL;
   if (!wsid || !wsid[0] || !cJSON_IsObject(r))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "missing workspace_id or response object");
   }
   cJSON *owned = cJSON_Duplicate(r, 1);
   int rc = owned ? ws_runner_registry_respond(wsid, owned) : -1;
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 404, "no runner registered for workspace");
   snprintf(resp, (size_t)cap, "{\"ok\":true}");
   return 200;
}

/* ── the registry ─────────────────────────────────────────────────────────
 * Rows are matched first-to-last; matches are mutually exclusive across
 * (verb, path, suffix), so order is not significant for correctness. */
static const http_route_t g_v1_routes[] = {
    /* Public: liveness, capability advertisement, model catalog, contract. */
    {"GET", "/v1/health", NULL, RM_EXACT, NULL, 0, rh_health},
    {"GET", "/v1/version", NULL, RM_EXACT, NULL, 0, rh_version},
    {"GET", "/v1/capabilities", NULL, RM_EXACT, NULL, 0, rh_capabilities},
    {"GET", "/v1/models", NULL, RM_EXACT, NULL, 0, rh_models},
    {"GET", "/v1/openapi.json", NULL, RM_EXACT, NULL, 0, rh_openapi},
    {"GET", "/v1/openapi.yaml", NULL, RM_EXACT, NULL, 0, rh_openapi},

    /* Reads / queries — cap derived from the NDJSON twin where one exists, else
     * a direct dashboard/session-read classification. */
    {"GET", "/v1/rules", NULL, RM_EXACT, "rules.list", 0, rh_rules},
    {"GET", "/v1/dashboard/memory", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_dashboard_memory},
    {"GET", "/v1/dashboard/reminders", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ,
     rh_dashboard_reminders},
    {"GET", "/v1/kb/status", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_kb_status},
    {"GET", "/v1/kb/curator", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_kb_curator},
    {"GET", "/v1/agents", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_agents},
    {"GET", "/v1/roadmap", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_roadmap},
    {"GET", "/v1/curiosity", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_curiosity},
    {"GET", "/v1/notes", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_notes},

    /* Native query POSTs. */
    {"POST", "/v1/kb/search", NULL, RM_EXACT, NULL, CAP_INDEX_READ, rh_kb_search},
    {"POST", "/v1/memory/recall", NULL, RM_EXACT, "memory.recall", 0, rh_memory_recall},
    {"POST", "/v1/notes/search", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_notes_search},

    /* Memory read family (hub-migration P1), dispatch-backed; caps derived from
     * the op (memory.* reads -> CAP_MEMORY_READ). memory.store (write) is not
     * exposed here. (memory.recall above keeps its bespoke native handler.) */
    {"POST", "/v1/memory/search", NULL, RM_EXACT, "memory.search", 0, rh_dispatch_op},
    {"POST", "/v1/memory/list", NULL, RM_EXACT, "memory.list", 0, rh_dispatch_op},
    {"GET", "/v1/memory/stats", NULL, RM_EXACT, "memory.stats", 0, rh_dispatch_op},
    {"POST", "/v1/memory/get", NULL, RM_EXACT, "memory.get", 0, rh_dispatch_op},
    {"GET", "/v1/memory/read", NULL, RM_EXACT, "memory.read", 0, rh_dispatch_op},

    /* Write families (hub-migration P1), dispatch-backed data-plane writes:
     * their ops are listed in g_v1_write_ops, so the TCP listener denies them at
     * remote_writes=off and allows them only at remote_writes=data/full after
     * capability checks. caps still derive from the op. */
    {"POST", "/v1/memory/store", NULL, RM_EXACT, "memory.store", 0, rh_dispatch_op},
    {"POST", "/v1/work/add", NULL, RM_EXACT, "work.add", 0, rh_dispatch_op},
    {"POST", "/v1/work/claim", NULL, RM_EXACT, "work.claim", 0, rh_dispatch_op},
    {"POST", "/v1/work/complete", NULL, RM_EXACT, "work.complete", 0, rh_dispatch_op},
    {"POST", "/v1/work/fail", NULL, RM_EXACT, "work.fail", 0, rh_dispatch_op},
    {"POST", "/v1/wm/set", NULL, RM_EXACT, "wm.set", 0, rh_dispatch_op},
    {"POST", "/v1/attempts/record", NULL, RM_EXACT, "attempt.record", 0, rh_dispatch_op},
    {"POST", "/v1/rules/delete", NULL, RM_EXACT, "rules.delete", 0, rh_dispatch_op},
    {"POST", "/v1/collab_rules/approve", NULL, RM_EXACT, "collab_rules.approve", 0, rh_dispatch_op},
    {"POST", "/v1/collab_rules/reject", NULL, RM_EXACT, "collab_rules.reject", 0, rh_dispatch_op},
    {"POST", "/v1/collab_rules/retire", NULL, RM_EXACT, "collab_rules.retire", 0, rh_dispatch_op},
    {"POST", "/v1/skills/create", NULL, RM_EXACT, "skill.create", 0, rh_dispatch_op},
    {"POST", "/v1/skills/edit", NULL, RM_EXACT, "skill.edit", 0, rh_dispatch_op},
    {"POST", "/v1/skills/archive", NULL, RM_EXACT, "skill.archive", 0, rh_dispatch_op},
    {"POST", "/v1/skills/pin", NULL, RM_EXACT, "skill.pin", 0, rh_dispatch_op},

    /* Code-index read family (hub-migration P1). First-class REST routes backed
     * by their NDJSON method twins through the server_dispatch loopback bridge
     * (rh_dispatch_op); cap derived from the op == CAP_INDEX_READ. index.scan
     * (a write) is intentionally not exposed here. */
    {"POST", "/v1/index/find", NULL, RM_EXACT, "index.find", 0, rh_dispatch_op},
    {"POST", "/v1/index/list", NULL, RM_EXACT, "index.list", 0, rh_dispatch_op},
    {"POST", "/v1/index/structure", NULL, RM_EXACT, "index.structure", 0, rh_dispatch_op},
    {"POST", "/v1/index/find_callers", NULL, RM_EXACT, "index.find_callers", 0, rh_dispatch_op},
    {"POST", "/v1/index/blast_radius", NULL, RM_EXACT, "index.blast_radius", 0, rh_dispatch_op},

    /* Skill + work-queue read families (hub-migration P1), same dispatch-backed
     * pattern; caps derived from the op (skill.list/show, work.list/stats are all
     * CAP_SESSION_READ). Mutating skill and work methods are not exposed here. */
    {"GET", "/v1/skills", NULL, RM_EXACT, "skill.list", 0, rh_dispatch_op},
    {"POST", "/v1/skills/show", NULL, RM_EXACT, "skill.show", 0, rh_dispatch_op},
    {"GET", "/v1/work", NULL, RM_EXACT, "work.list", 0, rh_dispatch_op},
    {"GET", "/v1/work/stats", NULL, RM_EXACT, "work.stats", 0, rh_dispatch_op},

    /* HUD status + trajectory export read families (hub-migration P1),
     * dispatch-backed; caps derived from the op (both CAP_SESSION_READ). */
    {"GET", "/v1/hud", NULL, RM_EXACT, "hud.status", 0, rh_dispatch_op},
    {"POST", "/v1/trajectory/export", NULL, RM_EXACT, "trajectory.export", 0, rh_dispatch_op},

    /* Toolset / collab-rule / working-memory / attempt-log / aux-config read
     * families (hub-migration P1), dispatch-backed; caps derived from the op
     * (CAP_SESSION_READ, except collab_rules reads which are CAP_RULES_READ).
     * Mutating twins (toolset writes, collab_rules approve/reject/retire, wm.set,
     * attempt.record, aux.test) are not exposed here. */
    {"GET", "/v1/toolsets", NULL, RM_EXACT, "toolset.list", 0, rh_dispatch_op},
    {"POST", "/v1/toolsets/show", NULL, RM_EXACT, "toolset.show", 0, rh_dispatch_op},

    /* Harness hooks + tool execution. These dispatch CAP_TOOL_EXECUTE methods, so
     * server_http_route_allowed gates them to remote_writes==full over the TCP
     * listener (privileged tier) while the local UDS — the co-located AI-tool
     * integration that actually drives them — bypasses as the trusted same-user
     * peer. Giving them first-class routes lets the CLI reach them without the
     * retired generic dispatch endpoint (op-parity: removed from
     * check-v1-method-coverage's EXCLUDED). */
    {"POST", "/v1/hooks/pre", NULL, RM_EXACT, "hooks.pre", 0, rh_dispatch_op},
    {"POST", "/v1/hooks/post", NULL, RM_EXACT, "hooks.post", 0, rh_dispatch_op},
    {"POST", "/v1/hooks/session_start", NULL, RM_EXACT, "hooks.session_start", 0, rh_dispatch_op},
    {"POST", "/v1/tools/execute", NULL, RM_EXACT, "tool.execute", 0, rh_dispatch_op},
    {"GET", "/v1/collab_rules", NULL, RM_EXACT, "collab_rules.list", 0, rh_dispatch_op},
    {"GET", "/v1/collab_rules/active", NULL, RM_EXACT, "collab_rules.list_active", 0,
     rh_dispatch_op},
    {"POST", "/v1/wm/list", NULL, RM_EXACT, "wm.list", 0, rh_dispatch_op},
    {"POST", "/v1/attempts/list", NULL, RM_EXACT, "attempt.list", 0, rh_dispatch_op},
    {"GET", "/v1/aux/config", NULL, RM_EXACT, "aux.config_show", 0, rh_dispatch_op},
    {"GET", "/v1/config", NULL, RM_EXACT, "config.show", 0, rh_dispatch_op},
    {"POST", "/v1/config/get", NULL, RM_EXACT, "config.get", 0, rh_dispatch_op},
    {"POST", "/v1/config/set", NULL, RM_EXACT, "config.set", 0, rh_dispatch_op},

    /* cron.* family (op-parity P1 wave 1) — single-response, object-body
     * methods, so each is a plain dispatch-op row. */
    {"GET", "/v1/cron", NULL, RM_EXACT, "cron.list", 0, rh_dispatch_op},
    {"POST", "/v1/cron/add", NULL, RM_EXACT, "cron.add", 0, rh_dispatch_op},
    {"POST", "/v1/cron/show", NULL, RM_EXACT, "cron.show", 0, rh_dispatch_op},
    {"POST", "/v1/cron/history", NULL, RM_EXACT, "cron.history", 0, rh_dispatch_op},
    {"POST", "/v1/cron/run", NULL, RM_EXACT, "cron.run", 0, rh_dispatch_op},
    {"POST", "/v1/cron/enable", NULL, RM_EXACT, "cron.enable", 0, rh_dispatch_op},
    {"POST", "/v1/cron/disable", NULL, RM_EXACT, "cron.disable", 0, rh_dispatch_op},
    {"POST", "/v1/cron/remove", NULL, RM_EXACT, "cron.remove", 0, rh_dispatch_op},

    /* delegate.* / job.* / jobs.* / agent.* / episode.* (op-parity wave 2).
     * launch/start/setup return a job id immediately (async), so they are safe
     * for the inline dispatch bridge; none stream. */
    {"GET", "/v1/delegate/log", NULL, RM_EXACT, "delegate.log", 0, rh_dispatch_op},
    {"GET", "/v1/delegate/backend_list", NULL, RM_EXACT, "delegate.backend_list", 0,
     rh_dispatch_op},
    {"POST", "/v1/delegate/status", NULL, RM_EXACT, "delegate.status", 0, rh_dispatch_op},
    /* Credential vault (WP-C.1): the vault gates on the attested UDS principal
     * (a TCP caller gets no principal and the service refuses the root-key push),
     * so these are dispatched through the same inline bridge as the delegate
     * methods. None stream. */
    {"POST", "/v1/vault/unlock", NULL, RM_EXACT, "vault.unlock", 0, rh_dispatch_op},
    {"POST", "/v1/vault/rekey", NULL, RM_EXACT, "vault.rekey", 0, rh_dispatch_op},
    {"POST", "/v1/vault/set", NULL, RM_EXACT, "vault.set", 0, rh_dispatch_op},
    {"POST", "/v1/vault/set_server", NULL, RM_EXACT, "vault.set_server", 0, rh_dispatch_op},
    {"POST", "/v1/vault/capability", NULL, RM_EXACT, "vault.capability", 0, rh_dispatch_op},
    {"POST", "/v1/vault/list", NULL, RM_EXACT, "vault.list", 0, rh_dispatch_op},
    {"POST", "/v1/vault/delete", NULL, RM_EXACT, "vault.delete", 0, rh_dispatch_op},
    {"POST", "/v1/vault/lock", NULL, RM_EXACT, "vault.lock", 0, rh_dispatch_op},
    {"POST", "/v1/cert/issue", NULL, RM_EXACT, "cert.issue", 0, rh_dispatch_op},
    {"POST", "/v1/cert/list", NULL, RM_EXACT, "cert.list", 0, rh_dispatch_op},
    {"POST", "/v1/cert/revoke", NULL, RM_EXACT, "cert.revoke", 0, rh_dispatch_op},
    /* Background-only over /v1: the thin client forces `background` (returns a
     * job_id to poll via the /v1/jobs routes); a foreground delegate streams and
     * would tie up a listener thread, so remote callers use background mode. */
    {"POST", "/v1/delegate/run", NULL, RM_EXACT, "delegate", 0, rh_dispatch_op},
    {"POST", "/v1/delegate/reply", NULL, RM_EXACT, "delegate.reply", 0, rh_dispatch_op},
    {"POST", "/v1/delegate/launch", NULL, RM_EXACT, "delegate.launch", 0, rh_dispatch_op},
    {"POST", "/v1/delegate/backend_exec", NULL, RM_EXACT, "delegate.backend_exec", 0,
     rh_dispatch_op},
    {"GET", "/v1/job/list", NULL, RM_EXACT, "job.list", 0, rh_dispatch_op},
    {"POST", "/v1/job/start", NULL, RM_EXACT, "job.start", 0, rh_dispatch_op},
    {"POST", "/v1/job/status", NULL, RM_EXACT, "job.status", 0, rh_dispatch_op},
    {"POST", "/v1/job/cancel", NULL, RM_EXACT, "job.cancel", 0, rh_dispatch_op},
    {"GET", "/v1/jobs/list", NULL, RM_EXACT, "jobs.list", 0, rh_dispatch_op},
    {"POST", "/v1/jobs/status", NULL, RM_EXACT, "jobs.status", 0, rh_dispatch_op},
    {"POST", "/v1/jobs/logs", NULL, RM_EXACT, "jobs.logs", 0, rh_dispatch_op},
    {"POST", "/v1/jobs/cancel", NULL, RM_EXACT, "jobs.cancel", 0, rh_dispatch_op},
    {"GET", "/v1/agent/list", NULL, RM_EXACT, "agent.list", 0, rh_dispatch_op},
    {"GET", "/v1/agent/local", NULL, RM_EXACT, "agent.local", 0, rh_dispatch_op},
    {"POST", "/v1/agent/add", NULL, RM_EXACT, "agent.add", 0, rh_dispatch_op},
    {"POST", "/v1/agent/remove", NULL, RM_EXACT, "agent.remove", 0, rh_dispatch_op},
    {"POST", "/v1/agent/enable", NULL, RM_EXACT, "agent.enable", 0, rh_dispatch_op},
    {"POST", "/v1/agent/disable", NULL, RM_EXACT, "agent.disable", 0, rh_dispatch_op},
    {"POST", "/v1/agent/probe", NULL, RM_EXACT, "agent.probe", 0, rh_dispatch_op},
    {"POST", "/v1/agent/setup", NULL, RM_EXACT, "agent.setup", 0, rh_dispatch_op},
    {"POST", "/v1/agent/setup_poll", NULL, RM_EXACT, "agent.setup_poll", 0, rh_dispatch_op},
    {"POST", "/v1/agent/cli_oauth_start", NULL, RM_EXACT, "agent.cli_oauth_start", 0,
     rh_dispatch_op},
    {"POST", "/v1/agent/cli_oauth_code", NULL, RM_EXACT, "agent.cli_oauth_code", 0, rh_dispatch_op},
    {"POST", "/v1/agent/cli_oauth_poll", NULL, RM_EXACT, "agent.cli_oauth_poll", 0, rh_dispatch_op},
    {"POST", "/v1/agent/episodes", NULL, RM_EXACT, "agent.episodes", 0, rh_dispatch_op},
    {"GET", "/v1/episode/list", NULL, RM_EXACT, "episode.list", 0, rh_dispatch_op},

    /* provider.* / model.* / api.* (op-parity wave 3). */
    {"GET", "/v1/provider/list", NULL, RM_EXACT, "provider.list", 0, rh_dispatch_op},
    {"GET", "/v1/provider/models", NULL, RM_EXACT, "provider.models", 0, rh_dispatch_op},
    {"POST", "/v1/provider/get", NULL, RM_EXACT, "provider.get", 0, rh_dispatch_op},
    {"POST", "/v1/provider/show", NULL, RM_EXACT, "provider.show", 0, rh_dispatch_op},
    {"POST", "/v1/provider/set", NULL, RM_EXACT, "provider.set", 0, rh_dispatch_op},
    {"POST", "/v1/provider/quota", NULL, RM_EXACT, "provider.quota", 0, rh_dispatch_op},
    {"POST", "/v1/provider/test", NULL, RM_EXACT, "provider.test", 0, rh_dispatch_op},
    {"POST", "/v1/provider/slot_acquire", NULL, RM_EXACT, "provider.slot_acquire", 0,
     rh_dispatch_op},
    {"POST", "/v1/provider/slot_release", NULL, RM_EXACT, "provider.slot_release", 0,
     rh_dispatch_op},
    {"GET", "/v1/model/list", NULL, RM_EXACT, "model.list", 0, rh_dispatch_op},
    {"GET", "/v1/model/show", NULL, RM_EXACT, "model.show", 0, rh_dispatch_op},
    {"POST", "/v1/model/refresh", NULL, RM_EXACT, "model.refresh", 0, rh_dispatch_op},
    {"GET", "/v1/api/status", NULL, RM_EXACT, "api.status", 0, rh_dispatch_op},
    {"POST", "/v1/api/enable", NULL, RM_EXACT, "api.enable", 0, rh_dispatch_op},
    {"POST", "/v1/api/disable", NULL, RM_EXACT, "api.disable", 0, rh_dispatch_op},

    /* dashboard.* / insights.* / identity.* / dogfood.* / lsp.* (op-parity wave 4).
     * Dashboard surfaces are read views (GET). */
    {"GET", "/v1/dashboard/all", NULL, RM_EXACT, "dashboard.all", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/delegations", NULL, RM_EXACT, "dashboard.delegations", 0,
     rh_dispatch_op},
    {"GET", "/v1/dashboard/logs", NULL, RM_EXACT, "dashboard.logs", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/memory_stats", NULL, RM_EXACT, "dashboard.memory_stats", 0,
     rh_dispatch_op},
    {"GET", "/v1/dashboard/metrics", NULL, RM_EXACT, "dashboard.metrics", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/onboard", NULL, RM_EXACT, "dashboard.onboard", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/plans", NULL, RM_EXACT, "dashboard.plans", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/plugins", NULL, RM_EXACT, "dashboard.plugins", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/traces", NULL, RM_EXACT, "dashboard.traces", 0, rh_dispatch_op},
    {"GET", "/v1/plugins", NULL, RM_EXACT, "plugin.list", 0, rh_dispatch_op},
    {"POST", "/v1/plugins/enable", NULL, RM_EXACT, "plugin.enable", 0, rh_dispatch_op},
    {"POST", "/v1/plugins/disable", NULL, RM_EXACT, "plugin.disable", 0, rh_dispatch_op},
    {"GET", "/v1/insights/overview", NULL, RM_EXACT, "insights.overview", 0, rh_dispatch_op},
    {"GET", "/v1/optimize/export", NULL, RM_EXACT, "optimize.export", 0, rh_dispatch_op},
    {"POST", "/v1/optimize/promote", NULL, RM_EXACT, "optimize.promote", 0, rh_dispatch_op},
    {"POST", "/v1/optimize/replay-record", NULL, RM_EXACT, "optimize.replay_record", 0,
     rh_dispatch_op},
    {"GET", "/v1/calibration/readiness", NULL, RM_EXACT, "calibration.readiness", 0,
     rh_dispatch_op},
    {"GET", "/v1/demotion/check", NULL, RM_EXACT, "demotion.check", 0, rh_dispatch_op},
    {"GET", "/v1/identity/show", NULL, RM_EXACT, "identity.show", 0, rh_dispatch_op},
    {"POST", "/v1/identity/diff", NULL, RM_EXACT, "identity.diff", 0, rh_dispatch_op},
    {"POST", "/v1/identity/snapshot", NULL, RM_EXACT, "identity.snapshot", 0, rh_dispatch_op},
    {"GET", "/v1/dogfood/report", NULL, RM_EXACT, "dogfood.report", 0, rh_dispatch_op},
    {"POST", "/v1/dogfood/review", NULL, RM_EXACT, "dogfood.review", 0, rh_dispatch_op},
    {"POST", "/v1/dogfood/tag", NULL, RM_EXACT, "dogfood.tag", 0, rh_dispatch_op},
    {"POST", "/v1/lsp/diagnostics_summary", NULL, RM_EXACT, "lsp.diagnostics_summary", 0,
     rh_dispatch_op},

    /* curator.* (queries) / graph.explain / trajectory.batch (op-parity wave 5).
     * Long-running kb.build/ingest/update, graph.sync_code, index.scan,
     * memory.benchmark and curator.synthesize are deliberately NOT inline-
     * dispatched here (they would stall the single-threaded /v1 listener) —
     * see docs/v1-op-parity-buildout.md; they belong on async /v1/runs. */
    {"POST", "/v1/curator/contradictions", NULL, RM_EXACT, "curator.contradictions", 0,
     rh_dispatch_op},
    {"POST", "/v1/curator/implements", NULL, RM_EXACT, "curator.implements", 0, rh_dispatch_op},
    {"POST", "/v1/curator/invalidated", NULL, RM_EXACT, "curator.invalidated", 0, rh_dispatch_op},
    {"POST", "/v1/graph/explain", NULL, RM_EXACT, "graph.explain", 0, rh_dispatch_op},
    {"POST", "/v1/code/audit", NULL, RM_EXACT, "code.audit", 0, rh_dispatch_op},
    {"POST", "/v1/audit/trace", NULL, RM_EXACT, "evidence.trace_retrieval_event", 0,
     rh_dispatch_op},
    {"POST", "/v1/audit/provenance", NULL, RM_EXACT, "evidence.provenance_retrieval_event", 0,
     rh_dispatch_op},
    {"POST", "/v1/audit/fidelity", NULL, RM_EXACT, "evidence.fidelity_retrieval_event", 0,
     rh_dispatch_op},
    {"POST", "/v1/css/signals", NULL, RM_EXACT, "css.signals", 0, rh_dispatch_op},
    {"POST", "/v1/trajectory/batch", NULL, RM_EXACT, "trajectory.batch", 0, rh_dispatch_op},

    /* work.* / wm.* / skill.* / session.* / toolset.* / mcp.* / trigger.* and
     * assorted singletons (op-parity wave 6). Reuses the existing plural family
     * prefixes (/v1/skills, /v1/sessions, /v1/workspaces, /v1/toolsets); these
     * exact paths do not collide with the suffixed {id} prefix routes.
     * rules.generate and eval.run are deliberately excluded (LLM/long-running —
     * see docs/v1-op-parity-buildout.md). */
    {"POST", "/v1/work/add_batch", NULL, RM_EXACT, "work.add_batch", 0, rh_dispatch_op},
    {"GET", "/v1/work/board", NULL, RM_EXACT, "work.board", 0, rh_dispatch_op},
    {"POST", "/v1/work/cancel", NULL, RM_EXACT, "work.cancel", 0, rh_dispatch_op},
    {"POST", "/v1/work/clear", NULL, RM_EXACT, "work.clear", 0, rh_dispatch_op},
    {"POST", "/v1/work/gc", NULL, RM_EXACT, "work.gc", 0, rh_dispatch_op},
    {"POST", "/v1/work/release", NULL, RM_EXACT, "work.release", 0, rh_dispatch_op},
    {"POST", "/v1/work/sync_proposals", NULL, RM_EXACT, "work.sync_proposals", 0, rh_dispatch_op},
    {"POST", "/v1/wm/context", NULL, RM_EXACT, "wm.context", 0, rh_dispatch_op},
    {"POST", "/v1/wm/get", NULL, RM_EXACT, "wm.get", 0, rh_dispatch_op},
    {"POST", "/v1/skills/autostub", NULL, RM_EXACT, "skill.autostub", 0, rh_dispatch_op},
    {"POST", "/v1/skills/eval", NULL, RM_EXACT, "skill.eval", 0, rh_dispatch_op},
    {"POST", "/v1/skills/lifecycle", NULL, RM_EXACT, "skill.lifecycle", 0, rh_dispatch_op},
    {"POST", "/v1/skills/lint", NULL, RM_EXACT, "skill.lint", 0, rh_dispatch_op},
    {"POST", "/v1/skills/patch", NULL, RM_EXACT, "skill.patch", 0, rh_dispatch_op},
    {"POST", "/v1/skills/unpin", NULL, RM_EXACT, "skill.unpin", 0, rh_dispatch_op},
    {"POST", "/v1/sessions/create", NULL, RM_EXACT, "session.create", 0, rh_dispatch_op},
    {"POST", "/v1/sessions/close", NULL, RM_EXACT, "session.close", 0, rh_dispatch_op},
    {"POST", "/v1/sessions/get", NULL, RM_EXACT, "session.get", 0, rh_dispatch_op},
    {"POST", "/v1/sessions/brief", NULL, RM_EXACT, "session.brief", 0, rh_dispatch_op},
    {"GET", "/v1/sessions/presence", NULL, RM_EXACT, "session.presence", 0, rh_dispatch_op},
    {"POST", "/v1/toolsets/resolve", NULL, RM_EXACT, "toolset.resolve", 0, rh_dispatch_op},
    {"POST", "/v1/blast_radius/preview", NULL, RM_EXACT, "blast_radius.preview", 0, rh_dispatch_op},
    {"GET", "/v1/mcp/tools_list", NULL, RM_EXACT, "mcp.tools_list", 0, rh_dispatch_op},
    {"POST", "/v1/mcp/audit", NULL, RM_EXACT, "mcp.audit", 0, rh_dispatch_op},
    {"POST", "/v1/mcp/recheck", NULL, RM_EXACT, "mcp.recheck", 0, rh_dispatch_op},
    {"POST", "/v1/mcp/call", NULL, RM_EXACT, "mcp.call", 0, rh_dispatch_op},
    {"POST", "/v1/workspaces/context", NULL, RM_EXACT, "workspace.context", 0, rh_dispatch_op},
    {"POST", "/v1/worktree/gc", NULL, RM_EXACT, "worktree.gc", 0, rh_dispatch_op},
    {"POST", "/v1/aux/test", NULL, RM_EXACT, "aux.test", 0, rh_dispatch_op},
    {"GET", "/v1/eval/results", NULL, RM_EXACT, "eval.results", 0, rh_dispatch_op},
    {"GET", "/v1/trigger/list", NULL, RM_EXACT, "trigger.list", 0, rh_dispatch_op},
    {"POST", "/v1/trigger/status", NULL, RM_EXACT, "trigger.status", 0, rh_dispatch_op},
    {"POST", "/v1/trigger/fire", NULL, RM_EXACT, "trigger.fire", 0, rh_dispatch_op},
    {"POST", "/v1/trigger/cancel", NULL, RM_EXACT, "trigger.cancel", 0, rh_dispatch_op},
    {"POST", "/v1/init/run", NULL, RM_EXACT, "init.run", 0, rh_dispatch_op},
    {"POST", "/v1/launch/run", NULL, RM_EXACT, "launch.run", 0, rh_dispatch_op},
    {"GET", "/v1/server/info", NULL, RM_EXACT, "server.info", 0, rh_dispatch_op},
    {"GET", "/v1/server/health", NULL, RM_EXACT, "server.health", 0, rh_dispatch_op},

    /* Long-running / LLM methods, exposed async (rh_dispatch_op_async): each
     * returns a queued run handle; poll GET /v1/runs/{id}. They must NOT use the
     * inline rh_dispatch_op bridge — that would stall the single /v1 listener. */
    {"POST", "/v1/kb/build", NULL, RM_EXACT, "kb.build", 0, rh_dispatch_op_async},
    {"POST", "/v1/kb/ingest", NULL, RM_EXACT, "kb.ingest", 0, rh_dispatch_op_async},
    {"POST", "/v1/kb/update", NULL, RM_EXACT, "kb.update", 0, rh_dispatch_op_async},
    {"POST", "/v1/graph/sync_code", NULL, RM_EXACT, "graph.sync_code", 0, rh_dispatch_op_async},
    {"POST", "/v1/index/scan", NULL, RM_EXACT, "index.scan", 0, rh_dispatch_op_async},
    {"POST", "/v1/index/ingest", NULL, RM_EXACT, "index.ingest", 0, rh_dispatch_op_async},
    {"POST", "/v1/memory/benchmark", NULL, RM_EXACT, "memory.benchmark", 0, rh_dispatch_op_async},
    {"POST", "/v1/curator/synthesize", NULL, RM_EXACT, "curator.synthesize", 0,
     rh_dispatch_op_async},
    {"POST", "/v1/rules/generate", NULL, RM_EXACT, "rules.generate", 0, rh_dispatch_op_async},
    {"POST", "/v1/eval/run", NULL, RM_EXACT, "eval.run", 0, rh_dispatch_op_async},
    {"POST", "/v1/delegate/aggregate", NULL, RM_EXACT, "delegate.aggregate", 0,
     rh_dispatch_op_async},
    {"POST", "/v1/delegate/roundtable", NULL, RM_EXACT, "delegate.roundtable", 0,
     rh_dispatch_op_async},

    /* Compute / inference — consume model budget; map to the chat twin's cap. */
    {"POST", "/v1/chat/completions", NULL, RM_EXACT, "chat.send_stream", 0, rh_chat},
    {"POST", "/v1/completions", NULL, RM_EXACT, "chat.send_stream", 0, rh_completions},
    {"POST", "/v1/responses", NULL, RM_EXACT, "chat.send_stream", 0, rh_responses},
    {"POST", "/v1/embeddings", NULL, RM_EXACT, NULL, CAP_CHAT, rh_embeddings},
    /* Anthropic Messages API ingress (Claude Code). Buffered row handles
     * stream:false; the stream:true case is dispatched by handle_conn (this row
     * still drives the capability gate + conformance scan for it). */
    {"POST", "/v1/messages", NULL, RM_EXACT, "chat.send_stream", 0, rh_messages},
    {"POST", "/v1/messages/count_tokens", NULL, RM_EXACT, NULL, CAP_CHAT, rh_count_tokens},
    /* Streaming chat over HTTP: dispatched by handle_conn; row exists for the
     * capability gate + conformance scan. */
    {"POST", "/v1/chat/stream", NULL, RM_EXACT, "chat.send_stream", 0, NULL},

    /* Server-hosted Claude PTY session (terminal forwarding). All consume model
     * budget (spawn/drive claude), so they map to the chat capability. The
     * /stream row carries no handler — handle_conn offloads it to an SSE worker.
     * The prefix rows precede nothing ambiguous: the one-segment <id> rule keeps
     * /input and /resize and bare DELETE distinct. */
    {"POST", "/v1/cli/session", NULL, RM_EXACT, NULL, CAP_CHAT, rh_cli_session_create},
    {"POST", "/v1/cli/session/", "/input", RM_PREFIX, NULL, CAP_CHAT, rh_cli_session_input},
    {"POST", "/v1/cli/session/", "/resize", RM_PREFIX, NULL, CAP_CHAT, rh_cli_session_resize},
    {"GET", "/v1/cli/session/", "/stream", RM_PREFIX, NULL, CAP_CHAT, NULL},
    {"DELETE", "/v1/cli/session/", NULL, RM_PREFIX, NULL, CAP_CHAT, rh_cli_session_delete},

    /* Runs: create (compute), cancel (compute), live events (read, streamed by
     * handle_conn), status (read). The /events and /stop rows precede the bare
     * /v1/runs/<id> row; the one-segment <id> rule keeps them unambiguous. */
    {"POST", "/v1/runs", NULL, RM_EXACT, NULL, CAP_CHAT, rh_runs_post},
    {"POST", "/v1/runs/", "/stop", RM_PREFIX, NULL, CAP_CHAT, rh_runs_stop},
    {"GET", "/v1/runs/", "/events", RM_PREFIX, NULL, CAP_SESSION_READ, NULL},
    {"GET", "/v1/runs/", NULL, RM_PREFIX, NULL, CAP_SESSION_READ, rh_runs_get},

    /* Personas: read (session-read), create/edit/delete (session-admin config). */
    {"GET", "/v1/persona", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_persona_current},
    {"GET", "/v1/personas", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_personas_list},
    {"POST", "/v1/personas", NULL, RM_EXACT, NULL, CAP_SESSION_ADMIN, rh_personas_create},
    {"GET", "/v1/personas/", NULL, RM_PREFIX, NULL, CAP_SESSION_READ, rh_persona_show},
    {"PUT", "/v1/personas/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN, rh_persona_put},
    {"DELETE", "/v1/personas/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN, rh_persona_delete},

    /* Delegate role templates: read like personas, mutate as admin. */
    {"GET", "/v1/role_templates", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_role_templates_list},
    {"GET", "/v1/role_templates/", NULL, RM_PREFIX, NULL, CAP_SESSION_READ, rh_role_template_show},
    {"PUT", "/v1/role_templates/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN, rh_role_template_put},
    {"DELETE", "/v1/role_templates/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN,
     rh_role_template_delete},

    /* Unified presence: list / attach / detach / persona / events are
     * session-scoped on the owner's own presence. */
    /* Workflow visual composer (W7): read+author the wfe_ definition model and
     * read work-item run-state. Reads (incl. validate, which never mutates) are
     * dashboard-read; save is session-admin (authoring write). */
    {"GET", "/v1/workflow/blocks", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_blocks},
    {"GET", "/v1/workflow/defs", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_list},
    {"GET", "/v1/workflow/defs/", NULL, RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_get},
    {"POST", "/v1/workflow/validate", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_validate},
    {"POST", "/v1/workflow/save", NULL, RM_EXACT, NULL, CAP_SESSION_ADMIN, rh_wf_save},
    {"GET", "/v1/workflow/items", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_items},
    {"GET", "/v1/workflow/items/", NULL, RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_item},

    {"GET", "/v1/sessions", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_sessions_list},
    {"POST", "/v1/sessions/", "/attach", RM_PREFIX, NULL, CAP_SESSION_READ, rh_session_attach},
    {"POST", "/v1/sessions/", "/detach", RM_PREFIX, NULL, CAP_SESSION_READ, rh_session_detach},
    {"GET", "/v1/sessions/", "/persona", RM_PREFIX, NULL, CAP_SESSION_READ, rh_session_persona_get},
    {"POST", "/v1/sessions/", "/persona", RM_PREFIX, NULL, CAP_SESSION_ADMIN,
     rh_session_persona_set},
    {"GET", "/v1/sessions/", "/events", RM_PREFIX, NULL, CAP_SESSION_READ, NULL},
    /* Per-session primary agent selection. NOTE: preserves the pre-registry
     * behavior of caps 0 at the route gate (the prior hand-written cap ladder
     * did not list /primary); tightening this is a follow-up for the
     * capability-matrix owner. */
    {"GET", "/v1/sessions/", "/primary", RM_PREFIX, NULL, 0, rh_session_primary_get},
    /* primary select/clear: match the NDJSON method cap (primary.* = CAP_SESSION_READ
     * in server_auth.c); previously hardcoded 0, leaving the route reachable by a
     * bearer with no capability at all. */
    {"POST", "/v1/sessions/", "/primary", RM_PREFIX, NULL, CAP_SESSION_READ,
     rh_session_primary_set},
    {"DELETE", "/v1/sessions/", "/primary", RM_PREFIX, NULL, CAP_SESSION_READ,
     rh_session_primary_clear},

    /* Workspace resource plane (workspace-resource-plane §1): instance-scoped
     * registry. list/get are index:read; register/remove are tool:execute
     * (write) — capability-gated but NOT in g_v1_write_ops, so a detached
     * client with a write bearer can register/remove over TCP. {id} is the
     * percent-encoded absolute workspace path. */
    {"GET", "/v1/workspaces", NULL, RM_EXACT, "workspace.list", 0, rh_dispatch_op},
    {"POST", "/v1/workspaces", NULL, RM_EXACT, "workspace.add", 0, rh_workspaces_register},
    {"POST", "/v1/workspace/clone", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_workspace_clone},
    {"POST", "/v1/workspaces/", "/forge-token", RM_PREFIX, NULL, CAP_TOOL_EXECUTE,
     rh_workspace_forge_token},
    {"GET", "/v1/workspaces/", NULL, RM_PREFIX, "workspace.get", 0, rh_workspace_get},
    {"DELETE", "/v1/workspaces/", NULL, RM_PREFIX, "workspace.remove", 0, rh_workspace_remove},

    /* Detached-runner reverse channel over /v1: the fs-authority client serving
     * a detached workspace polls/responds here. tool:execute (the caller is the
     * fs/exec authority); NOT local-only — a remote serving client drives them
     * over the authenticated TCP listener. */
    {"POST", "/v1/runner/poll", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_runner_poll},
    {"POST", "/v1/runner/respond", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_runner_respond},

    {NULL, NULL, NULL, RM_EXACT, NULL, 0, NULL}};

/* Find the registry row for (method, path). On an RM_PREFIX match, the dynamic
 * <id> segment is copied into id_out[id_cap] (NUL-terminated, truncated to
 * fit). Returns NULL when no row matches. Pure: no sockets, no globals. */
static const http_route_t *route_match(const char *method, const char *path, char *id_out,
                                       size_t id_cap)
{
   if (id_out && id_cap)
      id_out[0] = '\0';
   for (int i = 0; g_v1_routes[i].verb; i++)
   {
      const http_route_t *e = &g_v1_routes[i];
      if (strcmp(method, e->verb) != 0)
         continue;
      if (e->kind == RM_EXACT)
      {
         if (strcmp(path, e->path) == 0)
            return e;
         continue;
      }
      size_t plen = strlen(e->path);
      if (strncmp(path, e->path, plen) != 0)
         continue;
      const char *rest = path + plen;
      const char *slash = strchr(rest, '/');
      const char *id_end;
      if (e->suffix)
      {
         if (!slash || strcmp(slash, e->suffix) != 0)
            continue;
         id_end = slash;
      }
      else
      {
         if (!rest[0] || slash)
            continue;
         id_end = rest + strlen(rest);
      }
      if (id_out && id_cap)
      {
         size_t idlen = (size_t)(id_end - rest);
         if (idlen >= id_cap)
            idlen = id_cap - 1;
         memcpy(id_out, rest, idlen);
         id_out[idlen] = '\0';
      }
      return e;
   }
   return NULL;
}

/* Backing implementation for server_http_route_caps (declared earlier). */
static uint32_t v1_route_caps_lookup(const char *method, const char *path)
{
   if (!method || !path)
      return 0;
   const http_route_t *e = route_match(method, path, NULL, 0);
   if (!e)
      return 0;
   return e->op ? server_capability_for_method(e->op) : e->caps;
}

/* Data-plane mutating NDJSON method twins exposed as /v1 routes. At
 * remote_writes=off they are local-UDS-only; remote_writes=data/full exposes
 * them over TCP after capability checks. Keeping write-ness keyed on the op —
 * rather than a per-row struct field — avoids a missing-initializer churn across
 * every existing read row under -Wextra. Add a data-write route's op here when
 * you add the row. */
static const char *const g_v1_write_ops[] = {"memory.store",
                                             "work.add",
                                             "work.claim",
                                             "work.complete",
                                             "work.fail",
                                             "wm.set",
                                             "attempt.record",
                                             "rules.delete",
                                             "collab_rules.approve",
                                             "collab_rules.reject",
                                             "collab_rules.retire",
                                             "skill.create",
                                             "skill.edit",
                                             "skill.archive",
                                             "skill.pin",
                                             NULL};

/* Backing implementation for v1_route_is_local_only (declared earlier):
 * historical name; returns whether the route dispatches a data-write op in
 * g_v1_write_ops. */
static int v1_route_is_local_only(const char *method, const char *path)
{
   if (!method || !path)
      return 0;
   const http_route_t *e = route_match(method, path, NULL, 0);
   if (!e || !e->op)
      return 0;
   for (int i = 0; g_v1_write_ops[i]; i++)
      if (strcmp(e->op, g_v1_write_ops[i]) == 0)
         return 1;
   return 0;
}

/* Backing implementation for server_http_route (declared earlier). Streaming
 * routes carry handler == NULL and are dispatched by handle_conn before this is
 * reached, so an unhandled match here is a 404 like an unknown path. */
static int v1_route_dispatch(const char *method, const char *path, const char *body, int body_len,
                             char *resp, int resp_cap)
{
   if (!method || !path || !resp || resp_cap <= 0)
      return err_json(resp, resp_cap, 400, "bad request");
   char id[256];
   const http_route_t *e = route_match(method, path, id, sizeof(id));
   if (!e || !e->handler)
      return err_json(resp, resp_cap, 404, "not found");
   route_req_t rq = {method, path, body, body_len, id, e->op};
   return e->handler(&rq, resp, resp_cap);
}
