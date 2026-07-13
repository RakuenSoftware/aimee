/* server_http_routes.c: split from server_http.c into a real translation unit
 * (was server_http_routes.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_http_internal.h"
#include "server_http.h"
#include "server.h"         /* CAP_* / CAPS_* capability bits, server_capability_for_method */
#include "server_conn_io.h" /* transport-aware fd I/O (native-TLS phase 1) */
#include "server_tls.h"     /* native TLS termination (phase 1b) */
#include "workspace_runner_registry.h" /* ws_runner_registry_poll/_respond for the /v1 reverse channel */
#include "forge_credentials.h"         /* forge_cred_install for the /v1 token-install route */
#include <time.h>
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup, aimee_base64_* */
#include "cli_session_pty.h"
#include "config.h"
#include "prompts.h"
#include "delegate_role.h"
#include "log.h"
#include "aimee_version.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include "openapi_server_data.h" /* AIMEE_OPENAPI_SERVER_YAML_STR (generated from api/openapi-server-v1.yaml) */
#include "openai_runs_store.h"
#include "roundtable_pipeline_capture.h" /* pipeline op-run capture seam (#18/#20) */
#include "presence.h"
#include "request_context.h"
#include "server_http_identity.h" /* WP-C.0 attested-identity capture/threading */
#include "server_workflow_api.h"  /* W7: /v1/workflow read+author handlers */
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
/* Route-handler deps used below but not needed by server_http.c's own body
 * (kept here, not in server_http.c, to respect its 2000-line limit). */
#include "git_forge_vault.h" /* GIT_FORGE_VAULT_AGENT/SSHKEY_CRED — per-webuser ssh-key vault */
#include "git_host_cred.h"   /* per-host git credential store for /v1/git/credentials */
#include "git_ops.h"         /* git_ops_run for /v1/workspace/git (WP-E) */
#include "git_ssh_agent.h"   /* git_ssh_agent_stop — drop live key handles on revoke */
#include "vault_service.h"   /* vault_service_set/delete for the per-webuser ssh-key route */
#include "git_project.h"     /* git_project_clone for /v1/workspace/clone (WP-D) */
#include "git_org_repos.h"   /* git_org_repos_list for /v1/workspace/org-repos */
#include "webuser_editor.h"  /* webuser_editor_ensure for /v1/workspace/editor (WP-I) */
#include "workspace_scope.h" /* ws_scope_user_root — project workspace root */
#include "webchat_live.h"    /* db1_webchat_live_get — the browser's live-turn poll */
#include "index.h"           /* index_scan_project after a webuser clone (WP-D) */
#include "aimee_home.h"      /* aimee_home — proposal artifact dir for /v1/dev/submit */
#include <math.h>            /* isfinite — validate the /v1/dev/submit budget cap */
#include <errno.h>           /* strtol overflow detection for /v1/dev/submit caps */
#include "wfe_engine.h"      /* wfe_work_item_create — POST /v1/dev/submit intake */
#include "json_fluent.h"     /* jo_cstr — parse the CI-event webhook body */
#include <openssl/hmac.h>    /* HMAC-SHA256 for the CI-event webhook (server links -lcrypto) */
#include "router_advise.h"   /* S4: router_autonomous_pick/_audit for dev-submit parity */
#include "wfe_scheduler.h"   /* wfe_scheduler_notify — resume the autonomy driver */
#include "wfe_approval.h"    /* wfe_approval_record/present — human-gate approval */
#include "wfe_store.h"       /* db1_work_item_* — gate approve/reject */
#include <sys/stat.h>        /* mkdir for the proposal artifact dir */
#include <time.h>            /* unique proposal artifact filename */

/* route_req_t + route_handler_fn now live in server_http_internal.h (shared so
 * server_ci_route.c can define its own handler). */

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

/* POST /v1/dev/submit {proposal_md, workflow?, repo?} — autonomous development
 * intake. Seeds the proposal as a work-item artifact, creates an AUTONOMOUS wfe
 * work item on the chosen workflow (default "build"), and notifies the scheduler
 * to drive it server-side. The run proceeds independent of this connection;
 * human gates park it for approval. Returns {work_item_id}. This is the ONLY way
 * an autonomous run begins — aimee never self-initiates. */
static int rh_dev_submit(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = rq->body ? cJSON_Parse(rq->body) : NULL;
   cJSON *jprop = body ? cJSON_GetObjectItemCaseSensitive(body, "proposal_md") : NULL;
   cJSON *jwf = body ? cJSON_GetObjectItemCaseSensitive(body, "workflow") : NULL;
   cJSON *jrepo = body ? cJSON_GetObjectItemCaseSensitive(body, "repo") : NULL;
   const char *proposal = (jprop && cJSON_IsString(jprop)) ? jprop->valuestring : "";
   const char *workflow_opt =
       (jwf && cJSON_IsString(jwf) && jwf->valuestring[0]) ? jwf->valuestring : NULL;
   const char *repo = (jrepo && cJSON_IsString(jrepo)) ? jrepo->valuestring : "";

   cJSON *out = NULL;
   char err[256] = "";
   int st = dev_submit_run(proposal, workflow_opt, repo, server_http_identity_principal(), &out,
                           err, sizeof err);
   cJSON_Delete(body);
   if (st == 200 && out)
   {
      char *s = cJSON_PrintUnformatted(out);
      snprintf(resp, cap, "%s", s ? s : "{}");
      free(s);
      cJSON_Delete(out);
      return 200;
   }
   cJSON_Delete(out);
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "error", err[0] ? err : "submit failed");
   char *s = cJSON_PrintUnformatted(e);
   snprintf(resp, cap, "%s", s ? s : "{\"error\":\"submit failed\"}");
   free(s);
   cJSON_Delete(e);
   return st;
}

/* Loop-back retry cap for operator rejects on a `retry_on_reject` gate: bounds an
 * operator who keeps rejecting from churning a work item forever (audit-row blowup
 * / resource exhaustion). Counted from the immutable lifecycle log, so no schema
 * change; at/above the cap a reject is forced terminal. */
#define GATE_MAX_REJECT_RETRIES 3

/* Count prior reject_retry loop-backs at THIS gate for this work item (per-gate
 * budget: the audit row's stage column is the gate). Returns -1 on a DB read error
 * so the caller fails CLOSED (a lifecycle-table outage must not silently lift the
 * cap, since gate_apply on the work_item table would still succeed). */
static int gate_count_reject_retries(const char *id, const char *gate)
{
   db1_lifecycle_event_t *ev = NULL;
   int n = db1_lifecycle_event_list(id, &ev);
   if (n < 0)
      return -1; /* fail closed */
   int c = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(ev[i].kind, "reject_retry") == 0 && strcmp(ev[i].stage, gate) == 0)
         c++;
   free(ev);
   return c;
}

/* POST /v1/workflow/items/<id>/gate {decision:"approve"|"reject", gate?} —
 * approve or reject the human gate a run is parked at. Operator-level
 * (CAP_WORKFLOW_ADMIN, outside CAPS_AUTHENTICATED) so a mere delegate/authenticated
 * bearer cannot drive a gate.
 *
 * The state change is applied via db1_work_item_gate_apply — a single guarded
 * UPDATE (WHERE current_stage + content_hash match AND pause_reason='pending_human')
 * so a double-action / reject-after-approve / concurrent-operator race cannot
 * double-apply: a precondition miss returns 409 instead of corrupting state.
 * Content is immutable while parked, so content_hash is the row's identity guard.
 *
 * Approve records a non-repudiable, content-hash-bound approval (HMAC-signed with
 * the operator key) then clears the pause. Reject is TERMINAL by default; a gate
 * may opt into loop-back via `retry_on_reject: true` (following its on_fail edge),
 * bounded by GATE_MAX_REJECT_RETRIES. Every decision appends a lifecycle audit
 * event (approve | reject | reject_retry); the append-only log is unsigned by
 * design (matching all other lifecycle events) — approvals carry their own signed
 * record for non-repudiation. The scheduler then resumes the run. */
static int rh_workflow_gate(const route_req_t *rq, char *resp, int cap)
{
   const char *id = rq->id;
   if (!id || !id[0])
   {
      snprintf(resp, cap, "{\"error\":\"missing work item id\"}");
      return 400;
   }
   db1_work_item_t wi;
   if (db1_work_item_get(id, &wi) != 1)
   {
      snprintf(resp, cap, "{\"error\":\"no such work item\"}");
      return 404;
   }
   cJSON *body = rq->body ? cJSON_Parse(rq->body) : NULL;
   cJSON *jdec = body ? cJSON_GetObjectItemCaseSensitive(body, "decision") : NULL;
   cJSON *jgate = body ? cJSON_GetObjectItemCaseSensitive(body, "gate") : NULL;
   /* Copy the decision OUT of `body` now: the cJSON doc is deleted before the
    * final response is formatted, so a pointer into it must not outlive it
    * (this was a use-after-free that echoed garbage in the success response). */
   char decision_buf[16] = "";
   if (jdec && cJSON_IsString(jdec) && jdec->valuestring)
      snprintf(decision_buf, sizeof decision_buf, "%s", jdec->valuestring);
   const char *decision = decision_buf;
   const char *actor = server_http_identity_principal();
   if (!actor || !actor[0])
      actor = "operator";

   /* The gate is ALWAYS the stage the row is currently parked at (read from the
    * row, never the request). It is the sole input to the retry decision and the
    * audit event, closing a confused-deputy where a request could name a different
    * (terminal-only) gate than the one actually parked. A request naming a
    * mismatching gate is refused rather than silently retargeted. */
   const char *gate = wi.current_stage;
   if (jgate && cJSON_IsString(jgate) && jgate->valuestring[0] &&
       strcmp(jgate->valuestring, gate) != 0)
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"run is parked at gate '%s', not '%s'\"}", gate,
               jgate->valuestring);
      return 409;
   }
   if (strcmp(decision, "approve") != 0 && strcmp(decision, "reject") != 0)
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"decision must be 'approve' or 'reject'\"}");
      return 400;
   }
   /* Approving a ROUNDTABLE gate is refused, mirroring wfe_gate_override's rail
    * (a human must not force-pass a panel): previously this endpoint recorded a
    * phantom approval + cleared the pause, and the engine simply re-ran the
    * gate — misleading the operator into thinking approve crossed it. The
    * levers for an escalated roundtable are: let the panel re-converge (the
    * pause clears on resume of the loop budget), raise the node's max_iters,
    * or reject. */
   if (strcmp(decision, "approve") == 0)
   {
      char ferr[256];
      wfe_def_t *gdef = wfe_load_workflow(wi.workflow_name, ferr, sizeof ferr);
      const wfe_node_t *gnode = gdef ? wfe_def_node(gdef, wi.current_stage) : NULL;
      int is_rt = gnode && gnode->block == WFE_BLK_GATE_ROUNDTABLE;
      if (gdef)
         wfe_def_free(gdef);
      if (is_rt)
      {
         cJSON_Delete(body);
         snprintf(resp, cap,
                  "{\"error\":\"'%s' is a roundtable gate — a human cannot force-pass a panel. "
                  "Resume re-runs the panel; reject terminates.\"}",
                  wi.current_stage);
         return 409;
      }
   }

   const char *result_kind = decision; /* audit kind; a reject may become reject_retry */
   char detail[256] = "";

   if (strcmp(decision, "approve") == 0)
   {
      /* Record the non-repudiable approval first (idempotent on a duplicate hash),
       * THEN atomically clear the pause — so the pause is never cleared without a
       * preceding signed approval. */
      int present = wfe_approval_present(id, gate, wi.content_hash);
      if (!present && wfe_approval_record(id, gate, wi.content_hash, actor) != 0)
      {
         cJSON_Delete(body);
         snprintf(resp, cap, "{\"error\":\"could not record approval\"}");
         return 500;
      }
      int g = db1_work_item_gate_apply(id, wi.current_stage, wi.content_hash, NULL, NULL);
      if (g < 0)
      {
         cJSON_Delete(body);
         snprintf(resp, cap, "{\"error\":\"gate apply failed\"}");
         return 500;
      }
      if (g == 0)
      {
         /* Not in the observed parked state. A prior identical approve having
          * already resumed it is idempotent success; anything else is a conflict. */
         cJSON_Delete(body);
         if (present)
         {
            snprintf(resp, cap,
                     "{\"work_item_id\":\"%s\",\"gate\":\"%s\",\"decision\":\"approve\"}", id,
                     gate);
            return 200;
         }
         snprintf(resp, cap, "{\"error\":\"run not parked at this gate (state changed)\"}");
         return 409;
      }
      /* No detail: the audit row's gate column already carries the stage. */
   }
   else /* reject — terminal by default; loop back only on an in-budget opt-in gate */
   {
      int retries = gate_count_reject_retries(id, gate); /* per-gate; -1 on DB error */
      const char *new_stage = NULL, *terminal_state = "rejected";
      char target[WFE_ID_LEN] = "";
      /* The atomic gate_apply below serializes rejects per parked state (a
       * successful loop-back changes current_stage + clears the pause, so any
       * concurrent reject hits the guard and 409s), so this pre-read retry count
       * cannot be raced past the cap. A negative count is a DB read error -> fail
       * closed (terminal), never silently unbounded. */
      if (retries < 0)
         LOG_WARN("server.workflow",
                  "gate reject %s/%s: retry count unavailable; failing closed (terminal)", id,
                  gate);
      else if (retries < GATE_MAX_REJECT_RETRIES)
      {
         char ferr[256];
         wfe_def_t *def = wfe_load_workflow(wi.workflow_name, ferr, sizeof ferr);
         if (def)
         {
            if (wfe_gate_reject_target(def, gate, target, sizeof target) == WFE_GATE_REJECT_RETRY)
            {
               new_stage = target;
               terminal_state = NULL;
               result_kind = "reject_retry";
            }
            wfe_def_free(def);
         }
         else /* unloadable def degrades to terminal, but loudly so ops can see it */
            LOG_WARN("server.workflow",
                     "gate reject %s/%s: workflow '%s' unloadable (%s); terminal", id, gate,
                     wi.workflow_name, ferr);
      }
      int g = db1_work_item_gate_apply(id, wi.current_stage, wi.content_hash, new_stage,
                                       terminal_state);
      if (g < 0)
      {
         cJSON_Delete(body);
         snprintf(resp, cap, "{\"error\":\"gate apply failed\"}");
         return 500;
      }
      if (g == 0)
      {
         cJSON_Delete(body);
         snprintf(resp, cap, "{\"error\":\"run not parked at this gate (state changed)\"}");
         return 409;
      }
      if (new_stage)
         snprintf(detail, sizeof detail, "from=%s to=%s", gate, new_stage);
      else if (retries < 0)
         snprintf(detail, sizeof detail, "stage=%s reason=retry_count_unavailable", gate);
      else if (retries >= GATE_MAX_REJECT_RETRIES)
         snprintf(detail, sizeof detail, "stage=%s reason=retry_cap_exceeded", gate);
      else
         snprintf(detail, sizeof detail, "stage=%s reason=rejected", gate);
   }

   cJSON_Delete(body);
   /* Append-only audit event (unsigned by design, like every lifecycle event;
    * non-repudiation for approvals lives in the signed approval record). The 6th
    * arg is content_hash (string); the 7th is cost (double). */
   db1_lifecycle_event_add(id, gate, result_kind, actor, detail, wi.content_hash, 0.0);
   wfe_scheduler_notify(); /* resume the run server-side */
   snprintf(resp, cap, "{\"work_item_id\":\"%s\",\"gate\":\"%s\",\"decision\":\"%s\"}", id, gate,
            result_kind);
   return 200;
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

static int rh_roundtables_list(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return route_roundtables_list(resp, cap);
}
static int rh_roundtables_create(const route_req_t *rq, char *resp, int cap)
{
   return route_roundtable_upsert(NULL, rq->body, resp, cap);
}
static int rh_roundtable_set_active(const route_req_t *rq, char *resp, int cap)
{
   return route_roundtable_set_active(rq->body, resp, cap);
}
static int rh_roundtable_show(const route_req_t *rq, char *resp, int cap)
{
   return route_roundtable_show(rq->id, resp, cap);
}
static int rh_roundtable_put(const route_req_t *rq, char *resp, int cap)
{
   return route_roundtable_upsert(rq->id, rq->body, resp, cap);
}
static int rh_roundtable_delete(const route_req_t *rq, char *resp, int cap)
{
   return route_roundtable_remove(rq->id, resp, cap);
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
static int rh_wf_block_put(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_block_put(rq->id, rq->body, resp, cap);
}
static int rh_wf_block_delete(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_block_delete(rq->id, resp, cap);
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
static int rh_wf_items_all(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   return wf_api_items_all(resp, cap);
}
static int rh_wf_item(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_item(rq->id, resp, cap);
}
/* rh_query_long is declared in server_http_internal.h and defined in
 * server_http_config_routes.c (moved there to keep this TU under the line cap). */
static int rh_wf_events(const route_req_t *rq, char *resp, int cap)
{
   long after = rh_query_long("after", 0);
   int limit = (int)rh_query_long("limit", 200);
   return wf_api_events(rq->id, after, limit, resp, cap);
}
static int rh_wf_proposal(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_proposal(rq->id, resp, cap);
}

/* The Workflow Actions lifecycle (rh_wf_item_pause/resume/stop/delete) and
 * project-file-browser (rh_wf_repo_tree/file) route adapters are defined in
 * server_http_config_routes.c (declared in server_http_internal.h) so this TU
 * stays under the line-check ceiling; the route table below references them. */

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

/* AIMEE_WEBCHAT_GIT=0 disables the whole webchat git surface — forge-token,
 * clone, ops, per-host credentials, ssh-key, projects, session-dir, OAuth —
 * without affecting the editor (which has its own AIMEE_WEBCHAT_EDITOR gate;
 * session-dir is git-panel-only and not on the editor path, so gating it here is
 * safe). On by default; only the exact value "0" disables it, so a blank/other
 * value leaves git enabled. Read per call (immediate single-byte compare, like
 * webuser_editor's AIMEE_WEBCHAT_EDITOR gate) — aimee never setenv()s at request
 * time, so there is no concurrent-mutation window. Each git route checks this
 * first and returns 503 when off — ahead of the 403 webuser check, which is
 * intentional: the disabled state is not a secret, and an operator can ship the
 * image with the editor but no git intake. */
static int git_surface_enabled(void)
{
   const char *v = getenv("AIMEE_WEBCHAT_GIT");
   return !(v && v[0] == '0' && v[1] == '\0');
}

static int rh_workspace_forge_token(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
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
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
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
   const cJSON *jtoken = cJSON_GetObjectItemCaseSensitive(body, "token");
   const char *url = (cJSON_IsString(jurl) && jurl->valuestring) ? jurl->valuestring : NULL;
   const char *name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : NULL;
   const char *token = (cJSON_IsString(jtoken) && jtoken->valuestring) ? jtoken->valuestring : NULL;

   char dest[MAX_PATH_LEN], pname[128], err[256];
   int rc = git_project_clone(principal, url, name, token, dest, sizeof(dest), pname, sizeof(pname),
                              err, sizeof(err));
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

/* GET /v1/workspace/org-repos?host=&owner= — list the repositories under an owner
 * on a git host (provider-agnostic: GitHub/GitLab/Gitea/Bitbucket), so the wizard
 * can bulk-clone a workspace. Auth is the attested webuser principal; enumeration
 * uses the per-host token from the sealed vault (or unauthenticated for a public
 * org). Nothing is cloned here. */
static int rh_workspace_org_repos(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   char host[256], owner[192];
   rh_query_str("host", host, sizeof(host));
   rh_query_str("owner", owner, sizeof(owner));

   cJSON *repos = NULL;
   char provider[32], err[256];
   int st = git_org_repos_list(host, owner, &repos, provider, sizeof(provider), err, sizeof(err));
   if (st != 0)
      return err_json(resp, cap, st, err);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "provider", provider);
   cJSON_AddItemToObject(out, "repos", repos); /* transfers ownership */
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/clone-org {host, owner, repos:[{name, clone_url}]} — bulk
 * clone a selection of a workspace's repos into the calling webuser's scoped tree.
 * Each repo is cloned via git_project_clone (identity + credential handling as
 * /v1/workspace/clone); individual failures do not abort the batch. Returns a
 * per-repo result list. */
static int rh_workspace_clone_org(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jrepos = cJSON_GetObjectItemCaseSensitive(body, "repos");
   if (!cJSON_IsArray(jrepos) || cJSON_GetArraySize(jrepos) == 0)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "repos[] required");
   }
   if (cJSON_GetArraySize(jrepos) > 100)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "too many repos (max 100 per request)");
   }

   cJSON *out = cJSON_CreateObject();
   cJSON *results = cJSON_AddArrayToObject(out, "results");
   const cJSON *jrepo = NULL;
   cJSON_ArrayForEach(jrepo, jrepos)
   {
      const cJSON *jname = cJSON_GetObjectItemCaseSensitive(jrepo, "name");
      const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(jrepo, "clone_url");
      const char *name = (cJSON_IsString(jname) && jname->valuestring) ? jname->valuestring : NULL;
      const char *url = (cJSON_IsString(jurl) && jurl->valuestring) ? jurl->valuestring : NULL;

      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "name", name ? name : "");
      char dest[MAX_PATH_LEN], pname[128], err[256];
      /* token=NULL → the host's stored credential (or server identity) is used. */
      int rc = url ? git_project_clone(principal, url, name, NULL, dest, sizeof(dest), pname,
                                       sizeof(pname), err, sizeof(err))
                   : -1;
      if (rc == 0)
      {
         index_scan_project(pname, dest, 0); /* best-effort: make it searchable */
         cJSON_AddBoolToObject(r, "ok", 1);
         cJSON_AddStringToObject(r, "project", pname);
         cJSON_AddNullToObject(r, "error");
      }
      else
      {
         cJSON_AddBoolToObject(r, "ok", 0);
         cJSON_AddNullToObject(r, "project");
         cJSON_AddStringToObject(r, "error", url ? err : "missing clone_url");
      }
      cJSON_AddItemToArray(results, r);
   }
   cJSON_Delete(body);

   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/git {project, op, message?, branch?, n?} — run a git
 * operation on the calling webchat user's project (webchat-git WP-E). Identity
 * is the attested X-Aimee-Webuser principal; the project + op are scoped +
 * allowlisted by git_ops, and remote ops use the user's vaulted creds. */
static int rh_workspace_git(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jproj = cJSON_GetObjectItemCaseSensitive(body, "project");
   const cJSON *jop = cJSON_GetObjectItemCaseSensitive(body, "op");
   const cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(body, "message");
   const cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(body, "branch");
   const cJSON *jn = cJSON_GetObjectItemCaseSensitive(body, "n");
   const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(body, "session_id");
   const char *project = (cJSON_IsString(jproj) && jproj->valuestring) ? jproj->valuestring : NULL;
   const char *op = (cJSON_IsString(jop) && jop->valuestring) ? jop->valuestring : NULL;
   /* Optional: run in the calling session's isolated worktree (same tree its
    * agent edits) rather than the shared project checkout. */
   const char *session_id = (cJSON_IsString(jsid) && jsid->valuestring) ? jsid->valuestring : NULL;
   /* text_arg is the commit message (commit), target branch (checkout), or the
    * PR title (pr; optional — empty → gh --fill from the branch's commits). */
   const char *text = NULL;
   if (op && (strcmp(op, "commit") == 0 || strcmp(op, "pr") == 0))
      text = (cJSON_IsString(jmsg) && jmsg->valuestring) ? jmsg->valuestring : NULL;
   else if (op && strcmp(op, "checkout") == 0)
      text = (cJSON_IsString(jbranch) && jbranch->valuestring) ? jbranch->valuestring : NULL;
   int num = (cJSON_IsNumber(jn)) ? (int)jn->valuedouble : 0;

   char *git_out = NULL, err[256];
   int rc = git_ops_run_session(principal, project, session_id, op, text, num, &git_out, err,
                                sizeof(err));
   cJSON_Delete(body);
   if (rc != 0)
   {
      free(git_out);
      return err_json(resp, cap, 400, err);
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "output", git_out ? git_out : "");
   free(git_out);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "git output too large");
}

/* GET /v1/workspace/projects — list the calling webchat user's projects (the
 * repos cloned under their scoped workspace). Identity is the attested
 * X-Aimee-Webuser principal, so a user only ever sees their own. */
static int rh_workspace_projects(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   char names[256][GIT_PROJECT_NAME_MAX];
   int count = git_project_list(principal, names, 256);
   if (count < 0)
      count = 0;
   cJSON *out = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(out, "projects");
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
   /* The user's scoped workspace root — used by the editor to open a project
    * folder (root/<project>) and by chat to set its working directory. It is the
    * caller's own workspace path, returned only to them. */
   char root[MAX_PATH_LEN];
   if (ws_scope_user_root(principal, 0, root, sizeof(root)) == 0)
      cJSON_AddStringToObject(out, "root", root);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/chat/live {session_id, since_rev} — the browser's polling source of
 * truth for an in-flight turn. The server mirrors the tmux pane scrape into the
 * db1 webchat_live row as the answer streams; the browser tails it on a fixed
 * ~500ms timer instead of reconciling the per-token SSE stream client-side (which
 * pegged a core). Returns the row ONLY when its monotonic rev advanced past
 * since_rev, so an unchanged turn returns changed:false and the browser does no
 * work. CAP_SESSION_READ (a session read, like /events). */
static int rh_chat_live(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(body, "session_id");
   const cJSON *jsince = cJSON_GetObjectItemCaseSensitive(body, "since_rev");
   const char *sid = (cJSON_IsString(jsid) && jsid->valuestring) ? jsid->valuestring : NULL;
   long long since = cJSON_IsNumber(jsince) ? (long long)jsince->valuedouble : -1;
   if (!sid || !sid[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "session_id required");
   }
   char *turn_id = NULL, *text = NULL, *status = NULL;
   long long rev = 0;
   int found = db1_webchat_live_get(sid, since, &turn_id, &text, &status, &rev);
   cJSON_Delete(body);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "changed", found == 1);
   if (found == 1)
   {
      cJSON_AddNumberToObject(out, "rev", (double)rev);
      cJSON_AddStringToObject(out, "turn_id", turn_id ? turn_id : "");
      cJSON_AddStringToObject(out, "text", text ? text : "");
      cJSON_AddStringToObject(out, "status", status ? status : "");
   }
   else
      cJSON_AddNumberToObject(out, "rev", (double)since); /* echo the cursor back */
   free(turn_id);
   free(text);
   free(status);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "live response too large");
}

/* POST /v1/workspace/session-dir {project, session_id} — resolve the absolute
 * directory the given session acts in for `project`: that session's isolated
 * sibling worktree (off the default branch, created on demand) when session_id is
 * present, else the project checkout. Lets the editor open the same tree the
 * session's agent edits. Identity is the attested webuser principal. */
static int rh_workspace_session_dir(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git projects require a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jproj = cJSON_GetObjectItemCaseSensitive(body, "project");
   const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(body, "session_id");
   const char *project = (cJSON_IsString(jproj) && jproj->valuestring) ? jproj->valuestring : NULL;
   const char *session_id = (cJSON_IsString(jsid) && jsid->valuestring) ? jsid->valuestring : NULL;

   char dir[MAX_PATH_LEN], err[256];
   int rc = git_ops_session_dir(principal, project, session_id, dir, sizeof(dir), err, sizeof(err));
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 400, err);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "dir", dir);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* POST /v1/workspace/editor — ensure the calling webchat user's code-server is
 * running and return its loopback port (webchat-git WP-I). Identity is the
 * attested X-Aimee-Webuser principal, so a user only ever drives their own
 * editor, rooted at their scoped workspace and launched with their vault-backed
 * git env. The port reaches only webchat (the trusted reverse-proxy, WP-J),
 * never the browser. 503 when the feature is disabled / code-server absent. */
static int rh_workspace_editor(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "the editor requires a webchat user");

   int port = 0;
   char err[256];
   int rc = webuser_editor_ensure(principal, &port, err, sizeof(err));
   if (rc == 0)
      return err_json(resp, cap, 503, "editor not available");
   if (rc < 0)
      return err_json(resp, cap, 500, err[0] ? err : "failed to start editor");

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddNumberToObject(out, "port", port);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

/* Per-host git credentials (single-user server, many providers). The calling
 * webchat user manages aimee-server's OWN stored tokens (one per host); the
 * secret is write-only over the API — listing returns host names only, never
 * tokens. Identity is the attested X-Aimee-Webuser principal. */
static int rh_git_credentials(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git credentials require a webchat user");

   /* GET → list configured hosts (no tokens). */
   if (strcmp(rq->method, "GET") == 0)
   {
      char hosts[64][GIT_HOST_MAX];
      int count = git_host_cred_list(hosts, 64);
      if (count < 0)
         count = 0;
      cJSON *out = cJSON_CreateObject();
      cJSON *arr = cJSON_AddArrayToObject(out, "hosts");
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(hosts[i]));
      char *s = cJSON_PrintUnformatted(out);
      int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
      free(s);
      cJSON_Delete(out);
      return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
   }

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jhost = cJSON_GetObjectItemCaseSensitive(body, "host");
   const cJSON *jtoken = cJSON_GetObjectItemCaseSensitive(body, "token");
   const char *host = (cJSON_IsString(jhost) && jhost->valuestring) ? jhost->valuestring : NULL;
   const char *token = (cJSON_IsString(jtoken) && jtoken->valuestring) ? jtoken->valuestring : NULL;
   /* Accept a full URL in `host` too (convenience) → reduce to its host. */
   char hostbuf[GIT_HOST_MAX];
   if (host && strstr(host, "://") && git_host_from_url(host, hostbuf, sizeof(hostbuf)))
      host = hostbuf;
   if (!host || !host[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "host required");
   }

   int rc;
   if (strcmp(rq->method, "DELETE") == 0)
   {
      rc = git_host_cred_delete(host);
      /* Revocation must leave no live credential handle behind (G5): the user's
       * running code-server baked the now-removed token into its env at spawn,
       * and the ssh-agent may hold a key loaded from the vault. Drop both AFTER
       * the vault entry is gone — order matters: deleting first means a /vscode
       * request that respawns the editor between these two steps re-reads an
       * already-empty vault, so it cannot pick the token back up. Both stop
       * functions are void + idempotent (no-op if nothing is running) so a
       * concurrent double-revoke is safe; webuser_editor_stop reaps the child
       * synchronously (bounded ~500ms), git_ssh_agent_stop only signals+unlinks.
       * Recycling is cheap — the editor respawns lazily on the next /vscode
       * request with a freshly-built (credential-free) env. */
      if (rc == 0)
      {
         webuser_editor_stop(principal);
         git_ssh_agent_stop(principal);
      }
   }
   else /* POST → set */
   {
      if (!token || !token[0])
      {
         cJSON_Delete(body);
         return err_json(resp, cap, 400, "token required");
      }
      rc = git_host_cred_set(host, token);
   }
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 500, "credential store failed");
   return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
              ? 200
              : err_json(resp, cap, 500, "too large");
}

/* Per-webuser SSH private key for git over SSH. Stored under the caller's OWN
 * principal (webuser:<name>, "git", "ssh_key") but sealed with the SERVER master
 * KEK (a server-only wrap), so the server can load it into the user's in-memory
 * ssh-agent autonomously (git_ssh_agent_ensure → git_forge_vault_sshkey) AND
 * storing it needs NO vault unlock — parity with per-host git tokens and delegate
 * keys. The key never reaches the browser (write-only) and never lands on disk.
 * The secret is never logged. */
static int rh_git_sshkey(const route_req_t *rq, char *resp, int cap)
{
   if (!git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "ssh keys require a webchat user");

   if (strcmp(rq->method, "DELETE") == 0)
   {
      /* Remove the stored key and drop any live agent handle (mirrors revoke). A
       * missing entry is success — DELETE is idempotent. git_ssh_agent_stop runs
       * only on a clean delete: on a real vault error the persisted key still
       * exists, so tearing the agent down would just force a reload — leaving it
       * is the less-inconsistent state. */
      vault_status_t st =
          vault_service_delete(principal, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED);
      if (st != VAULT_OK && st != VAULT_NO_ENTRY)
         return err_json(resp, cap, 500, "vault delete failed");
      git_ssh_agent_stop(principal);
      return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
                 ? 200
                 : err_json(resp, cap, 500, "too large");
   }
   if (strcmp(rq->method, "POST") != 0)
      return err_json(resp, cap, 405, "method not allowed");

   /* Bound the body before parsing: a private key is a few KB, so anything past
    * 64 KiB is abuse — fail fast rather than parse + re-wrap a huge blob. */
   if (rq->body_len > 65536)
      return err_json(resp, cap, 413, "ssh key too large");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jkey = cJSON_GetObjectItemCaseSensitive(body, "ssh_key");
   const char *key = (cJSON_IsString(jkey) && jkey->valuestring) ? jkey->valuestring : NULL;
   /* Cheap shape check so an obviously-wrong paste fails fast with a clear
    * message; ssh-add is still the authority at load time. Anchor on the PEM/
    * OpenSSH armor ("-----BEGIN … PRIVATE KEY-----") rather than a loose
    * substring so arbitrary text containing the words can't slip through. We do
    * NOT accept a passphrase-encrypted key — the agent loads non-interactively. */
   if (!key || strncmp(key, "-----BEGIN", 10) != 0 || !strstr(key, "PRIVATE KEY-----"))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "an unencrypted OpenSSH/PEM private key is required");
   }
   if (strstr(key, "ENCRYPTED") || strstr(key, "Proc-Type:"))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400,
                      "passphrase-encrypted keys are not supported; provide an unencrypted key");
   }

   /* Seal the key under the SERVER master KEK (not the caller's per-user KEK) so
    * storing needs NO vault unlock — parity with per-host git tokens and delegate
    * keys. It is read back the same way (git_forge_vault_sshkey ->
    * vault_service_get_server_wrap), so there is never a VAULT_ERR_LOCKED here. */
   vault_status_t st =
       vault_service_set_server_wrap(principal, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, key);
   /* Zero our parsed copy of the secret before freeing the JSON tree. */
   if (jkey && jkey->valuestring)
   {
      volatile char *p = (volatile char *)jkey->valuestring;
      for (size_t i = 0; p[i]; i++)
         p[i] = 0;
   }
   cJSON_Delete(body);
   if (st != VAULT_OK)
      return err_json(resp, cap, 500, "vault store failed");
   /* A freshly stored key supersedes any agent already running with the old one. */
   git_ssh_agent_stop(principal);
   return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
              ? 200
              : err_json(resp, cap, 500, "too large");
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
    {"POST", "/v1/index/deps", NULL, RM_EXACT, "index.deps", 0, rh_dispatch_op},
    {"POST", "/v1/repo/trust", NULL, RM_EXACT, "repo.trust", 0, rh_dispatch_op},
    {"POST", "/v1/index/blast_radius", NULL, RM_EXACT, "index.blast_radius", 0, rh_dispatch_op},

    /* Skill + work-queue read families (hub-migration P1), same dispatch-backed
     * pattern; caps derived from the op (skill.list/show, work.list/stats are all
     * CAP_SESSION_READ). Mutating skill and work methods are not exposed here. */
    {"GET", "/v1/skills", NULL, RM_EXACT, "skill.list", 0, rh_dispatch_op},
    {"POST", "/v1/skills/show", NULL, RM_EXACT, "skill.show", 0, rh_dispatch_op},
    {"GET", "/v1/hosts", NULL, RM_EXACT, "hosts.list", 0, rh_dispatch_op},
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
    /* Workspace-independent SessionStart brief for the remote thin client
     * (Proposal 1 Phase 1): side-effect-free build_session_context assembly,
     * distinct from /v1/sessions/brief (which reads a persisted brief). */
    {"POST", "/v1/session/brief_assemble", NULL, RM_EXACT, "session.brief_assemble", 0,
     rh_dispatch_op},
    {"POST", "/v1/memory/user_capture", NULL, RM_EXACT, "memory.user_capture", 0, rh_dispatch_op},
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
    {"POST", "/v1/agent/roles", NULL, RM_EXACT, "agent.roles", 0, rh_dispatch_op},
    {"POST", "/v1/agent/personas", NULL, RM_EXACT, "agent.personas", 0, rh_dispatch_op},
    {"POST", "/v1/agent/set", NULL, RM_EXACT, "agent.set", 0, rh_dispatch_op},
    {"POST", "/v1/agent/disable", NULL, RM_EXACT, "agent.disable", 0, rh_dispatch_op},
    {"POST", "/v1/agent/probe", NULL, RM_EXACT, "agent.probe", 0, rh_dispatch_op},
    {"GET", "/v1/agent/stats", NULL, RM_EXACT, "agent.stats", 0, rh_dispatch_op},
    {"POST", "/v1/agent/draft", NULL, RM_EXACT, "agent.draft", 0, rh_dispatch_op},
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
    {"POST", "/v1/api/rotate_bearer", NULL, RM_EXACT, "api.rotate_bearer", 0, rh_dispatch_op},
    {"POST", "/v1/api/disable", NULL, RM_EXACT, "api.disable", 0, rh_dispatch_op},
    /* dashboard/insights/identity/dogfood/lsp op-parity wave 4; read views are GET. */
    {"GET", "/v1/dashboard/all", NULL, RM_EXACT, "dashboard.all", 0, rh_dispatch_op},
    {"GET", "/v1/dashboard/audit", NULL, RM_EXACT, "dashboard.audit", 0, rh_dispatch_op},
    {"GET", "/v1/audit/verify", NULL, RM_EXACT, "audit.verify", 0, rh_dispatch_op},
    {"POST", "/v1/audit/checkpoint", NULL, RM_EXACT, "audit.checkpoint", 0, rh_dispatch_op},
    {"POST", "/v1/audit/seal", NULL, RM_EXACT, "audit.seal", 0, rh_dispatch_op},
    {"POST", "/v1/audit/snapshot", NULL, RM_EXACT, "audit.snapshot", 0, rh_dispatch_op},
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
    {"GET", "/v1/intelligence/ranker/export-view", NULL, RM_EXACT, "ranker.export_view", 0,
     rh_dispatch_op},
    {"POST", "/v1/intelligence/ranker/fit", NULL, RM_EXACT, "ranker.fit", 0, rh_dispatch_op},
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
    {"POST", "/v1/curator/stages", NULL, RM_EXACT, "curator.stages", 0, rh_dispatch_op},
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
    {"POST", "/v1/sessions/record_transcript", NULL, RM_EXACT, "session.record_transcript", 0,
     rh_dispatch_op},
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
    {"POST", "/v1/dev/sweep", NULL, RM_EXACT, "dev.sweep", 0, rh_dispatch_op_async},

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
    /* Steering: stop the in-flight turn + queue a follow-up the server
     * auto-continues (streamed to the session's /events). */
    {"POST", "/v1/chat/interrupt", NULL, RM_EXACT, "chat.interrupt", 0, rh_dispatch_op},
    /* The browser's fixed-timer poll for the live turn (db1 webchat_live mirror),
     * replacing client-side SSE reconciliation. */
    {"POST", "/v1/chat/live", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_chat_live},

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
    /* Autonomous development intake: submit a proposal for end-to-end execution
     * (CAP_DELEGATE — it spawns delegate work). The only way an autonomous run
     * begins; aimee never self-initiates. */
    {"POST", "/v1/dev/submit", NULL, RM_EXACT, NULL, CAP_DELEGATE, rh_dev_submit},
    /* CI webhook: HMAC-signed (own integrity), so it needs no capability bit — a
     * machine caller has no attested principal. */
    {"POST", "/v1/dev/ci-event", NULL, RM_EXACT, NULL, 0, rh_dev_ci_event},
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

    /* Named roundtable presets: read like personas, mutate as admin. The exact
     * POST /v1/roundtables/active (set active preset) precedes the prefix routes
     * so it is not captured as a preset name. */
    {"GET", "/v1/roundtables", NULL, RM_EXACT, NULL, CAP_SESSION_READ, rh_roundtables_list},
    {"POST", "/v1/roundtables", NULL, RM_EXACT, NULL, CAP_SESSION_ADMIN, rh_roundtables_create},
    {"POST", "/v1/roundtables/active", NULL, RM_EXACT, NULL, CAP_SESSION_ADMIN,
     rh_roundtable_set_active},
    {"GET", "/v1/roundtables/", NULL, RM_PREFIX, NULL, CAP_SESSION_READ, rh_roundtable_show},
    {"PUT", "/v1/roundtables/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN, rh_roundtable_put},
    {"DELETE", "/v1/roundtables/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN, rh_roundtable_delete},

    /* Unified presence: list / attach / detach / persona / events are
     * session-scoped on the owner's own presence. */
    /* Workflow visual composer (W7): read+author the wfe_ definition model and
     * read work-item run-state. Reads (incl. validate, which never mutates) are
     * dashboard-read; save is session-admin (authoring write). */
    {"GET", "/v1/workflow/blocks", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_blocks},
    {"PUT", "/v1/workflow/blocks/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN, rh_wf_block_put},
    {"DELETE", "/v1/workflow/blocks/", NULL, RM_PREFIX, NULL, CAP_SESSION_ADMIN,
     rh_wf_block_delete},
    {"GET", "/v1/workflow/defs", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_list},
    {"GET", "/v1/workflow/defs/", NULL, RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_get},
    {"POST", "/v1/workflow/validate", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_validate},
    {"POST", "/v1/workflow/save", NULL, RM_EXACT, NULL, CAP_SESSION_ADMIN, rh_wf_save},
    {"GET", "/v1/workflow/items", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_items},
    /* Operator view of ALL items (not submitter-scoped) — exact row precedes the
     * /<id> prefix row so "all" isn't parsed as a work-item id. */
    {"GET", "/v1/workflow/items/all", NULL, RM_EXACT, NULL, CAP_WORKFLOW_ADMIN, rh_wf_items_all},
    /* Operator-only: approve/reject the human gate a run is parked at. The
     * suffix rows precede the bare /<id> row. */
    {"POST", "/v1/workflow/items/", "/gate", RM_PREFIX, NULL, CAP_WORKFLOW_ADMIN, rh_workflow_gate},
    /* Proposal read surfaces (ownership enforced in-handler, not by the route cap):
     * the timeline and the source markdown, owner-scoped in wf_api_*. */
    {"GET", "/v1/workflow/items/", "/events", RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_events},
    {"GET", "/v1/workflow/items/", "/proposal", RM_PREFIX, NULL, CAP_DASHBOARD_READ,
     rh_wf_proposal},
    /* Lifecycle mutations. Route cap admits owners (CAP_DASHBOARD_READ); the
     * handler additionally allows operators (CAP_WORKFLOW_ADMIN) and 403s a
     * non-owner non-operator. Suffix rows precede the bare /<id> rows. */
    {"POST", "/v1/workflow/items/", "/pause", RM_PREFIX, NULL, CAP_DASHBOARD_READ,
     rh_wf_item_pause},
    {"POST", "/v1/workflow/items/", "/resume", RM_PREFIX, NULL, CAP_DASHBOARD_READ,
     rh_wf_item_resume},
    {"POST", "/v1/workflow/items/", "/stop", RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_item_stop},
    {"GET", "/v1/workflow/items/", NULL, RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_item},
    /* DELETE the bare /<id> — distinct from the GET row by verb; auto-stops an
     * active run then removes it. */
    {"DELETE", "/v1/workflow/items/", NULL, RM_PREFIX, NULL, CAP_DASHBOARD_READ, rh_wf_item_delete},
    /* Composer project-file browser (read-only, confined to the local checkout). */
    {"GET", "/v1/workflow/repo/tree", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_repo_tree},
    {"GET", "/v1/workflow/repo/file", NULL, RM_EXACT, NULL, CAP_DASHBOARD_READ, rh_wf_repo_file},

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
    {"POST", "/v1/workspace/git", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_workspace_git},
    {"POST", "/v1/workspace/session-dir", NULL, RM_EXACT, NULL, CAP_INDEX_READ,
     rh_workspace_session_dir},
    {"GET", "/v1/workspace/projects", NULL, RM_EXACT, NULL, CAP_INDEX_READ, rh_workspace_projects},
    {"GET", "/v1/workspace/org-repos", NULL, RM_EXACT, NULL, CAP_INDEX_READ,
     rh_workspace_org_repos},
    {"POST", "/v1/workspace/clone-org", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_workspace_clone_org},
    {"POST", "/v1/workspace/editor", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_workspace_editor},
    {"GET", "/v1/git/credentials", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_git_credentials},
    {"POST", "/v1/git/credentials", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_git_credentials},
    {"DELETE", "/v1/git/credentials", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_git_credentials},
    {"POST", "/v1/git/sshkey", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_git_sshkey},
    {"DELETE", "/v1/git/sshkey", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_git_sshkey},
    {"POST", "/v1/git/oauth/github/start", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_github_start},
    {"POST", "/v1/git/oauth/github/poll", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_github_poll},
    {"GET", "/v1/git/oauth/github/config", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_github_config},
    {"POST", "/v1/git/oauth/github/config", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_github_config},
    {"POST", "/v1/git/oauth/github/web/start", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_github_web_start},
    {"POST", "/v1/git/oauth/github/web/callback", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_github_web_callback},
    {"POST", "/v1/git/oauth/device/start", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_device_start},
    {"POST", "/v1/git/oauth/device/poll", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_device_poll},
    {"GET", "/v1/git/oauth/device/config", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_device_config},
    {"POST", "/v1/git/oauth/device/config", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE,
     rh_git_oauth_device_config},
    {"POST", "/v1/deploy/apply", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_deploy_apply},
    {"GET", "/v1/deploy/status", NULL, RM_EXACT, NULL, CAP_TOOL_EXECUTE, rh_deploy_status},
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
uint32_t v1_route_caps_lookup(const char *method, const char *path)
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
int v1_route_is_local_only(const char *method, const char *path)
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
int v1_route_dispatch(const char *method, const char *path, const char *body, int body_len,
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
