/* server_http.c: aimee-server inbound HTTP-over-UDS /v1 API (see server_http.h).
 *
 * Hand-rolled HTTP/1.1 server on a dedicated Unix socket, mirroring the
 * aimee-kb HTTP server. First resource: /v1/personas. */
/* _GNU_SOURCE: struct ucred / SO_PEERCRED peer-credential capture is a GNU
 * extension; declare it before any include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_http_internal.h"
#include <aimee/core/connection/auth.h>
#include "server_http.h"
#include "sandbox_pkg_proxy.h" /* delegate-sandbox package forward proxy (UDS demux) */
#include "kb_identity_token.h"
#include "server_write_tier.h"
#include "server_write_tier_db1.h"
#include "server.h"         /* CAP_* / CAPS_* capability bits, server_capability_for_method */
#include "server_conn_io.h" /* transport-aware fd I/O (native-TLS phase 1) */
#include "server_tls.h"     /* native TLS termination (phase 1b) */
#include "runtime_secret.h" /* Vault-sourced management private keys */
#include "server_mgmt_checkpoint_client.h"
#include "pki.h" /* P8a per-request durable cert revocation/expiry re-check */
#include "modules/workspace/workspace_runner_registry.h" /* ws_runner_registry_poll/_respond for the /v1 reverse channel */
#include <time.h>
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup, aimee_base64_* */
#include "cli_session_pty.h"
#include "config.h"
#include "prompts.h"
#include <aimee/delegates/delegate_role.h>
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
#include "server_http_authz.h"    /* capability/route-gate policy + per-user write tier */
#include "server_workflow_api.h"  /* W7: /v1/workflow read+author handlers */
#include "shadow_mirror.h"        /* generic shadow-traffic mirror */
#include "http_content_encoding.h"
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

/* SHTTP_MAX_BODY / SHTTP_MAX_ROUNDTABLE_BODY now live in headers/server.h so
 * clients can honour the same cap. */
#define SHTTP_BACKLOG 16
/* ── per-session persona store ──────────────────────────────────────────── */

#define SHTTP_MAX_SESSIONS 256

static struct
{
   char session_id[128];
   char persona[PERSONA_NAME_MAX];
   char active_primary[MAX_AGENT_NAME];
} g_sessions[SHTTP_MAX_SESSIONS];
static int g_session_count = 0;
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

void session_persona_set(const char *session_id, const char *persona)
{
   if (!session_id || !session_id[0] || !persona || !persona[0])
      return;
   pthread_mutex_lock(&g_session_lock);
   for (int i = 0; i < g_session_count; i++)
   {
      if (strcmp(g_sessions[i].session_id, session_id) == 0)
      {
         snprintf(g_sessions[i].persona, sizeof(g_sessions[i].persona), "%s", persona);
         pthread_mutex_unlock(&g_session_lock);
         return;
      }
   }
   int slot = (g_session_count < SHTTP_MAX_SESSIONS)
                  ? g_session_count++
                  : (SHTTP_MAX_SESSIONS - 1); /* overwrite last */
   snprintf(g_sessions[slot].session_id, sizeof(g_sessions[slot].session_id), "%s", session_id);
   snprintf(g_sessions[slot].persona, sizeof(g_sessions[slot].persona), "%s", persona);
   pthread_mutex_unlock(&g_session_lock);
}

int session_persona_get(const char *session_id, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!session_id || !session_id[0] || !out || !n)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_session_lock);
   for (int i = 0; i < g_session_count; i++)
   {
      if (strcmp(g_sessions[i].session_id, session_id) == 0)
      {
         snprintf(out, n, "%s", g_sessions[i].persona);
         found = 1;
         break;
      }
   }
   pthread_mutex_unlock(&g_session_lock);
   return found;
}

void session_primary_set(const char *session_id, const char *agent)
{
   if (!session_id || !session_id[0] || !agent || !agent[0])
      return;
   pthread_mutex_lock(&g_session_lock);
   for (int i = 0; i < g_session_count; i++)
   {
      if (strcmp(g_sessions[i].session_id, session_id) == 0)
      {
         snprintf(g_sessions[i].active_primary, sizeof(g_sessions[i].active_primary), "%s", agent);
         pthread_mutex_unlock(&g_session_lock);
         return;
      }
   }
   int slot = (g_session_count < SHTTP_MAX_SESSIONS)
                  ? g_session_count++
                  : (SHTTP_MAX_SESSIONS - 1); /* overwrite last */
   snprintf(g_sessions[slot].session_id, sizeof(g_sessions[slot].session_id), "%s", session_id);
   snprintf(g_sessions[slot].active_primary, sizeof(g_sessions[slot].active_primary), "%s", agent);
   pthread_mutex_unlock(&g_session_lock);
}

int session_primary_get(const char *session_id, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!session_id || !session_id[0] || !out || !n)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_session_lock);
   for (int i = 0; i < g_session_count; i++)
   {
      if (strcmp(g_sessions[i].session_id, session_id) == 0)
      {
         if (g_sessions[i].active_primary[0])
         {
            snprintf(out, n, "%s", g_sessions[i].active_primary);
            found = 1;
         }
         break;
      }
   }
   pthread_mutex_unlock(&g_session_lock);
   return found;
}

void session_primary_clear(const char *session_id)
{
   if (!session_id || !session_id[0])
      return;
   pthread_mutex_lock(&g_session_lock);
   for (int i = 0; i < g_session_count; i++)
   {
      if (strcmp(g_sessions[i].session_id, session_id) == 0)
      {
         g_sessions[i].active_primary[0] = '\0';
         break;
      }
   }
   pthread_mutex_unlock(&g_session_lock);
}

const char *server_http_delegate_block(const char *session_id, const char *role, const char *prompt,
                                       char *buf, size_t n)
{
   if (!buf || !n)
      return NULL;
   size_t plen = prompt ? strlen(prompt) : 0;
   if (plen == 0)
   {
      snprintf(buf, n, "missing prompt");
      return buf;
   }
   if (plen < 20)
   {
      snprintf(buf, n, "prompt too short (%zu chars)", plen);
      return buf;
   }
   if (!role || !role[0])
      return NULL;
   char pname[PERSONA_NAME_MAX] = "";
   if (!(session_id && session_id[0] && session_persona_get(session_id, pname, sizeof(pname))))
      config_current_persona(pname, sizeof(pname));
   persona_t p;
   persona_load(NULL, pname, &p);
   const char *out = NULL;
   if (!persona_delegates_enabled(&p))
   {
      snprintf(buf, n, "the '%s' persona does not use delegates; do the work yourself", p.name);
      out = buf;
   }
   else if (delegate_role_is_write(role) && !persona_delegates_writes(&p))
   {
      snprintf(buf, n, "the '%s' persona uses read-only delegates only; '%s' is a write role",
               p.name, role);
      out = buf;
   }
   persona_free(&p);
   return out;
}

/* ── JSON helpers ───────────────────────────────────────────────────────── */

int emit(char *resp, int cap, cJSON *obj)
{
   char *s = cJSON_PrintUnformatted(obj);
   if (s)
   {
      snprintf(resp, (size_t)cap, "%s", s);
      free(s);
   }
   else
      snprintf(resp, (size_t)cap, "{}");
   cJSON_Delete(obj);
   return 200;
}

int err_json(char *resp, int cap, int status, const char *msg)
{
   http_error_json(resp, (size_t)cap, msg ? msg : "error"); /* escapes msg → valid JSON */
   return status;
}

/* ── auth ───────────────────────────────────────────────────────────────── */

int server_http_authorize(int is_tcp, const char *bearer_cfg, const char *auth_header,
                          const char *api_key_header, int has_session_key)
{
   int have_bearer = bearer_cfg && bearer_cfg[0];
   int authorized = 0;

   /* Session-scoping is refused unless a bearer is configured — prevents an
    * unauthenticated session-scoping pivot, on any transport. */
   if (has_session_key && !have_bearer)
      return 503;

   if (!is_tcp)
      return 0; /* UDS: filesystem-permission auth, no token */

   /* TCP requires a configured bearer and a matching Authorization header. */
   if (!have_bearer)
      return 503;
   authorized = server_ct_equal(aimee_core_bearer_token(auth_header), bearer_cfg);
   if (api_key_header && api_key_header[0])
      authorized |= server_ct_equal(api_key_header, bearer_cfg);
   if (!authorized)
      return 401;
   return 0;
}

/* The declarative /v1 route registry (server_http_routes.inc, included below)
 * is the single source of truth for dispatch, per-route capabilities, and the
 * OpenAPI path inventory. server_http_route_caps and server_http_route are thin
 * public entry points over it; route matching lives in route_match(). */

void server_http_api_status_report(int http_port, int bearer_configured, int rate_limit_per_min,
                                   char *buf, size_t n)
{
   if (!buf || n == 0)
      return;
   size_t off = 0;
#define SAR_APPEND(...)                                                                            \
   do                                                                                              \
   {                                                                                               \
      if (off < n)                                                                                 \
         off += (size_t)snprintf(buf + off, n - off, __VA_ARGS__);                                 \
   } while (0)

   SAR_APPEND("aimee /v1 HTTP API\n");
   if (http_port <= 0)
   {
      SAR_APPEND("  listener:      disabled (aimee.api.http_port unset)\n\n");
      SAR_APPEND("To use aimee as a model in VS Code (Continue/Cline/Roo/Copilot BYOK),\n"
                 "run `aimee api enable`. The trusted local socket generates and seals the\n"
                 "bearer directly into Vault; no credential is written to aimee.yaml.\n"
                 "Then re-run `aimee api status` for ready-to-paste provider snippets.\n");
      buf[n - 1] = '\0';
      return;
   }

   SAR_APPEND("  listener:      enabled on server loopback at http://127.0.0.1:%d/v1\n", http_port);
   SAR_APPEND("  bearer token:  %s\n", bearer_configured
                                           ? "configured"
                                           : "NOT configured — run `aimee api enable` locally "
                                             "(the listener refuses to bind without it)");
   if (rate_limit_per_min > 0)
      SAR_APPEND("  rate limit:    %d req/min\n", rate_limit_per_min);
   else
      SAR_APPEND("  rate limit:    unlimited\n");

   /* Per-user write authorization (0.3.0). The counter is surfaced here because
    * a counter nobody can read is not a metric — an operator mid-cutover needs
    * to see how much traffic the retired global is no longer letting through.
    *
    * Whether AIMEE_SERVER_TEAM_ID is configured is deliberately NOT reported
    * here: that check lives in the db1-backed resolver, and reaching for it
    * would drag storage into a status summary. The server already logs an ERROR
    * naming the variable at startup, which is where an operator diagnosing
    * "reads work but every write is denied" will actually be looking. */
   unsigned long long ignored = (unsigned long long)server_http_global_ignored_count();
   int retired_setting = server_http_remote_writes();
   if (ignored || retired_setting != SERVER_REMOTE_WRITES_OFF)
      SAR_APPEND("  remote_writes: aimee.api.remote_writes NO LONGER AUTHORIZES; %llu request(s) "
                 "refused that it would formerly have allowed "
                 "(remote_writes.global_ignored)\n",
                 ignored);

   SAR_APPEND("\nVS Code model-provider setup (base URL + bearer key + model `aimee`):\n");
   SAR_APPEND(
       "  Continue / Cline / Roo Code:                 http://127.0.0.1:%d/v1   model aimee\n",
       http_port);
   SAR_APPEND(
       "  Copilot \"Manage Models\" (OpenAI-compatible): http://127.0.0.1:%d/v1   model aimee\n",
       http_port);
   SAR_APPEND("\nThe URLs above are reachable only on the server host. From another machine, "
              "tunnel the port first:\n"
              "  ssh -L %d:127.0.0.1:%d <server-host>\n"
              "Then keep using http://127.0.0.1:%d/v1 on the client.\n",
              http_port, http_port, http_port);
   SAR_APPEND("\nUse a project:-scoped bearer (scope:project:<id>:<secret>) so the editor can\n"
              "read and chat but cannot perform admin mutations.\n");
   buf[n - 1] = '\0';
#undef SAR_APPEND
}

/* Persona + role-template /v1 route handlers (kept in a sibling .inc for size). */

/* The session's active persona (set via POST below), falling back to the durable
 * default when the session has none — so a reconnecting client (e.g. webchat)
 * can render the current selection. */
int route_session_persona_get(const char *session_id, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   char name[PERSONA_NAME_MAX];
   if (!session_persona_get(session_id, name, sizeof(name)))
      snprintf(name, sizeof(name), "%s", aimee_mode_to_string(config_current_mode()));
   persona_t p;
   persona_load(NULL, name, &p);
   int rc = emit(resp, cap, persona_to_json(&p));
   persona_free(&p);
   return rc;
}

int route_session_persona_set(const char *session_id, const char *body, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *jn = req ? cJSON_GetObjectItemCaseSensitive(req, "name") : NULL;
   if (!cJSON_IsString(jn) || !jn->valuestring[0])
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "missing persona name");
   }
   char name[PERSONA_NAME_MAX];
   snprintf(name, sizeof(name), "%s", jn->valuestring);
   cJSON_Delete(req);

   if (!persona_is_builtin(name))
   {
      char path[PERSONA_PATH_MAX];
      if (persona_path(NULL, name, path, sizeof(path)) != 0)
         return err_json(resp, cap, 404, "no such persona");
   }
   session_persona_set(session_id, name);

   persona_t p;
   persona_load(NULL, name, &p);
   int rc = emit(resp, cap, persona_to_json(&p));
   persona_free(&p);
   return rc;
}

int route_session_primary_get(const char *session_id, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   char name[MAX_AGENT_NAME] = "";
   session_primary_get(session_id, name, sizeof(name));
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "agent", name);
   return emit(resp, cap, o);
}

int route_session_primary_set(const char *session_id, const char *body, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *jn = req ? cJSON_GetObjectItemCaseSensitive(req, "agent") : NULL;
   if (!cJSON_IsString(jn) || !jn->valuestring[0])
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "missing agent name");
   }
   char name[MAX_AGENT_NAME];
   snprintf(name, sizeof(name), "%s", jn->valuestring);
   cJSON_Delete(req);

   /* Validate the agent exists in the pool before pinning it. */
   agent_t agbuf;
   if (agent_registry_find(name, &agbuf) != 0)
      return err_json(resp, cap, 404, "no such agent");

   session_primary_set(session_id, name);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "agent", name);
   return emit(resp, cap, o);
}

int route_session_primary_clear(const char *session_id, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   session_primary_clear(session_id);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "agent", "");
   return emit(resp, cap, o);
}

/* ── unified-presence routes ────────────────────────────────────────────────
 * GET  /v1/sessions                  → list the owner's live presences
 * POST /v1/sessions/{id}/attach      → attach a surface (creates on first attach)
 * POST /v1/sessions/{id}/detach      → detach a surface
 * GET  /v1/sessions/{id}/events      → SSE presence-event stream (handled on the
 *                                      streaming path in handle_conn, not here)
 * See docs/proposals/accepted/aimee-unified-presence.md and presence.h. In the
 * local-first default the owner is the single local principal, so listing is
 * unscoped; multi-owner scoping is a distributed-mode-auth concern. */

int route_sessions_list(char *resp, int cap)
{
   /* presence_list_json always writes valid JSON (at least "[]"). */
   presence_list_json(NULL, resp, (size_t)cap);
   return 200;
}

int route_session_attach(const char *session_id, const char *body, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *js = req ? cJSON_GetObjectItemCaseSensitive(req, "surface") : NULL;
   if (!cJSON_IsString(js) || !js->valuestring[0])
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "missing surface");
   }
   char surface[PRESENCE_SURFACE_MAX];
   snprintf(surface, sizeof(surface), "%s", js->valuestring);

   const cJSON *jt = cJSON_GetObjectItemCaseSensitive(req, "target");
   const char *target = (cJSON_IsString(jt) && jt->valuestring[0]) ? jt->valuestring : NULL;
   const cJSON *jo = cJSON_GetObjectItemCaseSensitive(req, "owner");
   const char *owner = (cJSON_IsString(jo) && jo->valuestring[0]) ? jo->valuestring : NULL;
   const cJSON *jm = cJSON_GetObjectItemCaseSensitive(req, "subscribe_mask");
   unsigned mask = cJSON_IsNumber(jm) ? (unsigned)jm->valuedouble : (unsigned)PRESENCE_EV_ALL;
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "persistent");
   int persistent = cJSON_IsBool(jp) ? cJSON_IsTrue(jp) : 0;

   char attach_id[64];
   int ok = presence_attach(session_id, owner, surface, target, mask, persistent, attach_id,
                            sizeof(attach_id));
   cJSON_Delete(req);
   if (!ok)
      return err_json(resp, cap, 409, "attach refused (owner mismatch or registry full)");

   /* Build from the /v1/sessions/ prefix literal (a documented templated-path
    * prefix) rather than a single percent-s-bearing events-route literal, so
    * the api-conformance scanner does not mistake this URL builder for a route
    * declaration that lacks a spec entry. */
   char events_url[256];
   snprintf(events_url, sizeof(events_url), "%s%s/events", "/v1/sessions/", session_id);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "session_id", session_id);
   cJSON_AddStringToObject(o, "attach_id", attach_id);
   cJSON_AddStringToObject(o, "events_url", events_url);
   return emit(resp, cap, o);
}

int route_session_detach(const char *session_id, const char *body, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *ja = req ? cJSON_GetObjectItemCaseSensitive(req, "attach_id") : NULL;
   if (!cJSON_IsString(ja) || !ja->valuestring[0])
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "missing attach_id");
   }
   int ok = presence_detach(session_id, ja->valuestring);
   cJSON_Delete(req);
   if (!ok)
      return err_json(resp, cap, 404, "no such attachment");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddBoolToObject(o, "detached", 1);
   return emit(resp, cap, o);
}

/* ── service plumbing routes (HTTP-API phase 1) ─────────────────────────────
 * Liveness / version / capability discovery for the aimee-server /v1 surface,
 * mirroring the aimee-kb HTTP server (src/kb/http/kb_http.c) so a client can
 * probe either service the same way. */

int route_health(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"status\":\"ok\",\"service\":\"aimee-server\"}");
   return 200;
}

int route_version(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"version\":\"%s\",\"service\":\"aimee-server\"}", AIMEE_VERSION);
   return 200;
}

int route_capabilities(char *resp, int cap)
{
   /* The resources this HTTP surface currently serves; grows with the API. */
   snprintf(resp, (size_t)cap,
            "{\"capabilities\":[\"personas\",\"sessions\",\"models\",\"chat\",\"embeddings\","
            "\"responses\",\"rules\",\"kb\",\"memory\",\"notes\",\"dashboard\",\"agents\","
            "\"roadmap\",\"curiosity\",\"runs\",\"openapi\"],"
            "\"version\":\"%s\",\"service\":\"aimee-server\"}",
            AIMEE_VERSION);
   return 200;
}

/* GET /v1/ready — readiness, deliberately distinct from /v1/health (liveness).
 * `health` answers "this process is alive and serving HTTP"; `ready` answers
 * "this process can serve work right now". An orchestrator restarts on a
 * liveness failure but only drains on a readiness failure, so conflating the
 * two would restart a healthy process during a transient dependency outage.
 *
 * The provider serves a snapshot sampled OFF the request path, so this route
 * performs no dependency I/O and a wedged dependency cannot stall the listener.
 * Until a provider is registered every dependency is `unknown` and the answer
 * is 503: readiness fails closed, so an unsampled server is never advertised as
 * ready. The body keeps one shape in every case (including the unregistered and
 * provider-failed cases) so a client parsing `.ready` / `.dependencies` never
 * has to special-case a server that has not sampled yet. */
static server_http_ready_fn g_ready_fn = NULL;

void server_http_set_ready_provider(server_http_ready_fn fn)
{
   g_ready_fn = fn;
}

/* The fail-closed body, used whenever no provider answered for us. */
static int ready_unknown(char *resp, int cap, const char *reason)
{
   snprintf(resp, (size_t)cap,
            "{\"ready\":false,\"status\":\"unknown\",\"service\":\"aimee-server\","
            "\"reason\":\"%s\",\"dependencies\":{}}",
            reason);
   return 503;
}

int route_ready(char *resp, int cap)
{
   if (cap > 0)
      resp[0] = '\0';
   if (!g_ready_fn)
      return ready_unknown(resp, cap, "no readiness provider registered");

   int status = g_ready_fn(resp, cap);

   /* Fail closed on anything we cannot positively confirm. A provider is
    * trusted to sample, not to define the contract, so the route validates
    * rather than passes through:
    *   - only 200 and 503 are legal; any other status is a bug, not a state;
    *   - a 200 must actually carry ready:true, so a provider cannot advertise
    *     readiness with a body that says otherwise (or with no body at all);
    *   - a 503 must not carry ready:true, so the two halves cannot contradict.
    * Checking resp[0] alone would catch an empty body but not a contradictory
    * one, which is the case that actually matters here. */
   if (status != 200 && status != 503)
      return ready_unknown(resp, cap, "readiness provider returned an invalid status");
   if (resp[0] == '\0')
      return ready_unknown(resp, cap, "readiness provider wrote no body");

   int says_ready = (strstr(resp, "\"ready\":true") != NULL);
   if (status == 200 && !says_ready)
      return ready_unknown(resp, cap, "readiness provider returned 200 without ready:true");
   if (status == 503 && says_ready)
      return ready_unknown(resp, cap, "readiness provider returned 503 with ready:true");

   return status;
}

/* GET /v1/models — OpenAI-compatible model discovery. Always advertises the
 * local `aimee` model; when the server has registered a models provider, the
 * configured agent names are appended (the (provider,model) bindings a client
 * can target via the `model` field). The provider seam keeps the agent/config
 * dependency out of this unit and its test. */
static server_http_models_fn g_models_fn = NULL;
static server_http_models_raw_fn g_models_raw_fn = NULL;

void server_http_set_models_provider(server_http_models_fn fn)
{
   g_models_fn = fn;
}

void server_http_set_models_raw_provider(server_http_models_raw_fn fn)
{
   g_models_raw_fn = fn;
}

#define SHTTP_MODELS_MAX 64

int route_models(char *resp, int cap)
{
   /* A raw provider (e.g. the Codex `{models:[…]}` schema) takes precedence; it
    * writes the whole body. <0 means "not handled" → fall through to the list. */
   if (g_models_raw_fn)
   {
      int rlen = g_models_raw_fn(resp, cap);
      if (rlen >= 0)
         return 200;
   }

   char extra[SHTTP_MODELS_MAX][SERVER_HTTP_MODEL_ID_MAX];
   int n_extra = g_models_fn ? g_models_fn(extra, SHTTP_MODELS_MAX - 1) : 0;
   if (n_extra < 0)
      n_extra = 0;

   const char *ids[SHTTP_MODELS_MAX];
   int n = 0;
   ids[n++] = "aimee";
   for (int i = 0; i < n_extra && n < SHTTP_MODELS_MAX; i++)
      ids[n++] = extra[i];

   int len = openai_format_models_list(ids, n, "aimee", resp, cap);
   if (len < 0)
      return err_json(resp, cap, 500, "models list error");
   return 200;
}

/* ── OpenAI completion seam (handlers registered by the server at startup) ── */

server_http_completion_fn g_chat_handler = NULL;
server_http_completion_fn g_completion_handler = NULL;
server_http_completion_fn g_embeddings_handler = NULL;
server_http_completion_fn g_responses_handler = NULL;
static server_http_stream_fn g_chat_stream_handler = NULL;
static server_http_stream_fn g_completion_stream_handler = NULL;
static server_http_responses_stream_fn g_responses_stream_handler = NULL;
server_http_completion_fn g_messages_handler = NULL;
static server_http_responses_stream_fn g_messages_stream_handler = NULL;
server_http_completion_fn g_count_tokens_handler = NULL;

void server_http_set_chat_handler(server_http_completion_fn fn)
{
   g_chat_handler = fn;
}

void server_http_set_chat_stream_handler(server_http_stream_fn fn)
{
   g_chat_stream_handler = fn;
}

void server_http_set_completion_stream_handler(server_http_stream_fn fn)
{
   g_completion_stream_handler = fn;
}

void server_http_set_responses_stream_handler(server_http_responses_stream_fn fn)
{
   g_responses_stream_handler = fn;
}

void server_http_set_completion_handler(server_http_completion_fn fn)
{
   g_completion_handler = fn;
}

void server_http_set_embeddings_handler(server_http_completion_fn fn)
{
   g_embeddings_handler = fn;
}

void server_http_set_responses_handler(server_http_completion_fn fn)
{
   g_responses_handler = fn;
}

void server_http_set_messages_handler(server_http_completion_fn fn)
{
   g_messages_handler = fn;
}

void server_http_set_messages_stream_handler(server_http_responses_stream_fn fn)
{
   g_messages_stream_handler = fn;
}

void server_http_set_count_tokens_handler(server_http_completion_fn fn)
{
   g_count_tokens_handler = fn;
}

/* Native-resource providers/handlers (registered by server_native_register). */
server_http_json_provider g_rules_provider = NULL;
server_http_json_provider g_dashboard_memory_provider = NULL;
server_http_json_provider g_kb_status_provider = NULL;
server_http_json_provider g_agents_provider = NULL;
server_http_json_provider g_roadmap_provider = NULL;
server_http_json_provider g_curiosity_provider = NULL;
server_http_json_provider g_notes_list_provider = NULL;
server_http_json_provider g_dashboard_reminders_provider = NULL;
server_http_completion_fn g_kb_search_handler = NULL;
server_http_completion_fn g_memory_recall_handler = NULL;
server_http_completion_fn g_notes_search_handler = NULL;
server_http_completion_fn g_runs_handler = NULL;

void server_http_set_rules_provider(server_http_json_provider fn)
{
   g_rules_provider = fn;
}

void server_http_set_dashboard_memory_provider(server_http_json_provider fn)
{
   g_dashboard_memory_provider = fn;
}

void server_http_set_kb_status_provider(server_http_json_provider fn)
{
   g_kb_status_provider = fn;
}

void server_http_set_agents_provider(server_http_json_provider fn)
{
   g_agents_provider = fn;
}

void server_http_set_roadmap_provider(server_http_json_provider fn)
{
   g_roadmap_provider = fn;
}

void server_http_set_curiosity_provider(server_http_json_provider fn)
{
   g_curiosity_provider = fn;
}

void server_http_set_notes_list_provider(server_http_json_provider fn)
{
   g_notes_list_provider = fn;
}

void server_http_set_dashboard_reminders_provider(server_http_json_provider fn)
{
   g_dashboard_reminders_provider = fn;
}

void server_http_set_kb_search_handler(server_http_completion_fn fn)
{
   g_kb_search_handler = fn;
}

void server_http_set_memory_recall_handler(server_http_completion_fn fn)
{
   g_memory_recall_handler = fn;
}

void server_http_set_notes_search_handler(server_http_completion_fn fn)
{
   g_notes_search_handler = fn;
}

void server_http_set_runs_handler(server_http_completion_fn fn)
{
   g_runs_handler = fn;
}

/* GET /v1/runs/{id}: return the run record (404 when unknown, 410 when a valid
 * op-run was interrupted by replacement or evicted from this generation). The snapshot
 * reflects live status transitions (queued -> in_progress -> terminal) as the
 * background worker publishes them. */
static int run_missing_response(const char *id, char *resp, int cap)
{
   openai_runs_missing_t why = openai_runs_store_classify_missing(id);
   if (why == OPENAI_RUNS_MISSING_UNKNOWN)
      return err_json(resp, cap, 404, "no such run");
   const char *status = why == OPENAI_RUNS_MISSING_INTERRUPTED ? "interrupted" : "evicted";
   const char *message = why == OPENAI_RUNS_MISSING_INTERRUPTED
                             ? "run interrupted by server replacement"
                             : "run record evicted from live store";
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return err_json(resp, cap, 500, "out of memory");
   cJSON_AddStringToObject(o, "error", message);
   cJSON_AddStringToObject(o, "run_id", id);
   cJSON_AddStringToObject(o, "status", status);
   char *json = cJSON_PrintUnformatted(o);
   int n = json ? snprintf(resp, (size_t)cap, "%s", json) : -1;
   free(json);
   cJSON_Delete(o);
   return (n >= 0 && n < cap) ? 410 : err_json(resp, cap, 500, "response too large");
}

int route_runs_get(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err_json(resp, cap, 400, "missing run id");
   if (!openai_runs_store_get(id, resp, (size_t)cap))
      return run_missing_response(id, resp, cap);
   return 200;
}

/* POST /v1/runs/{id}/stop: request cancellation of an in-flight run. Sets the
 * cancel flag (a no-op once the run is already terminal); the background worker
 * observes it at its next step boundary and finalizes the run as "cancelled".
 * Returns the current run snapshot, 404 when unknown, or 410 when unavailable. */
int route_runs_stop(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err_json(resp, cap, 400, "missing run id");
   openai_run_status_t st;
   if (!openai_runs_store_status(id, &st))
      return run_missing_response(id, resp, cap);
   openai_runs_store_request_cancel(id);
   if (!openai_runs_store_get(id, resp, (size_t)cap))
      return run_missing_response(id, resp, cap);
   return 200;
}

/* Dispatch a native POST body to its handler; 503 (generic JSON error) when no
 * handler is wired in (e.g. unit tests, or kb_client not linked). */
int route_native_post(server_http_completion_fn fn, const char *body, char *resp, int cap,
                      const char *unavailable_msg)
{
   if (!fn)
      return err_json(resp, cap, 503, unavailable_msg);
   return fn(body ? body : "", resp, cap);
}

/* GET a native resource whose provider returns a heap JSON body (emitted +
 * freed here). 503 when unwired, 502 when the backend (aimee-kb) is
 * unreachable. `what` names the resource for the error messages. */
int route_json_provider(server_http_json_provider fn, char *resp, int cap, const char *what)
{
   if (!fn)
   {
      char msg[96];
      snprintf(msg, sizeof(msg), "%s is not available on this server", what);
      return err_json(resp, cap, 503, msg);
   }
   char *j = fn();
   if (!j)
   {
      char msg[96];
      snprintf(msg, sizeof(msg), "%s backend unavailable", what);
      return err_json(resp, cap, 502, msg);
   }
   snprintf(resp, (size_t)cap, "%s", j);
   free(j);
   return 200;
}

/* Dispatch a POST /v1/{chat/completions,completions} body to the registered
 * handler. Returns 503 (OpenAI-shaped) when no handler is wired in. */
int route_completion(server_http_completion_fn fn, const char *body, char *resp, int cap)
{
   if (!fn)
   {
      openai_format_error(resp, cap, "server_error",
                          "completions are not available on this server");
      return 503;
   }
   return fn(body ? body : "", resp, cap);
}

/* ── Dispatch-backed route connection caps ────────────────────────────────
 * The first-class /v1 routes (rh_dispatch_op / rh_dispatch_op_async) run their
 * NDJSON method twin through server_dispatch() via loopback_rpc, carrying a fake
 * connection with the request's real capabilities so server_dispatch re-checks
 * per-method caps. The standalone POST /v1/rpc bridge it grew out of was retired
 * once every method had a first-class route (op-parity complete). */

/* Capabilities for the request currently being routed, set by handle_conn from
 * the transport (UDS => CAPS_ALL; TCP => bearer-scoped). Thread-local: each
 * connection is handled on its own worker thread, and the route handlers read
 * this synchronously on that thread (the async dispatch-op path copies it into
 * its job before the worker runs). Defaults to CAPS_READ_ONLY so any direct
 * caller (e.g. unit tests) gets the conservative, read-only surface. */
_Thread_local uint32_t g_rpc_conn_caps = CAPS_READ_ONLY;

/* Dispatch an NDJSON {method,params} body through server_dispatch() over a
 * socketpair loopback and capture the response into resp. The write end is
 * non-blocking so an oversize response can never hang the server thread; if the
 * response is truncated it fails to parse and we return an error rather than a
 * partial body. Returns an HTTP status; resp holds the JSON response body. */
/* Upper bound on how long the dispatch bridge waits for a reply. Generous because
 * a deferred handler may be running a real tool, and safe because it blocks one
 * connection's own worker thread rather than the listener. */
#define LOOPBACK_REPLY_TIMEOUT_SECS 30

int loopback_rpc(const char *body, int body_len, char *resp, int resp_cap, uint32_t conn_caps)
{
   int sp[2];
   if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
      return err_json(resp, resp_cap, 500, "dispatch route unavailable");
   int fl = fcntl(sp[1], F_GETFL, 0);
   if (fl >= 0)
      fcntl(sp[1], F_SETFL, fl | O_NONBLOCK);

   server_conn_t fake;
   memset(&fake, 0, sizeof(fake));
   fake.fd = sp[1];
   fake.capabilities = conn_caps;
   /* WP-C.0 hop 2 of 3: the memset above zeroes the attested identity; restore
    * the real one captured by handle_conn so every /v1 request — which only ever
    * reaches server_dispatch through this synthesized conn — carries the caller's
    * vault principal through to create_compute_ctx (same thread, identity live). */
   server_http_identity_apply(&fake);
   pthread_mutex_init(&fake.mutex, NULL);
   pthread_cond_init(&fake.can_close, NULL);

   size_t mlen = body_len > 0 ? (size_t)body_len : strlen(body);
   server_dispatch(server_active_ctx(), &fake, body, mlen);

   pthread_mutex_destroy(&fake.mutex);
   pthread_cond_destroy(&fake.can_close);

   /* Deferred handlers reply through a dup, so bound the read instead of shutting it down. */
   struct timeval rcv = {LOOPBACK_REPLY_TIMEOUT_SECS, 0};
   (void)setsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

   size_t total = 0;
   for (;;)
   {
      ssize_t n = read(sp[0], resp + total, (size_t)resp_cap - 1 - total);
      if (n <= 0)
         break;
      total += (size_t)n;
      /* server_dispatch emits one NDJSON response. The newline is the message
       * boundary; another in-process reference may keep the socket alive, so
       * waiting for process-wide EOF can otherwise hang this adapter. */
      if (memchr(resp, '\n', total) != NULL)
         break;
      if (total >= (size_t)resp_cap - 1)
         break;
   }
   close(sp[0]);
   close(sp[1]);
   resp[total] = '\0';
   while (total > 0 && (resp[total - 1] == '\n' || resp[total - 1] == '\r'))
      resp[--total] = '\0';
   if (total == 0)
      return err_json(resp, resp_cap, 502, "rpc produced no response");
   cJSON *chk = cJSON_Parse(resp);
   if (!chk)
      return err_json(resp, resp_cap, 502, "rpc response too large or malformed");
   cJSON_Delete(chk);
   return 200;
}

int server_http_route(const char *method, const char *path, const char *body, int body_len,
                      char *resp, int resp_cap)
{
   return v1_route_dispatch(method, path, body, body_len, resp, resp_cap);
}

/* ── socket listener ────────────────────────────────────────────────────── */

static int g_listen_fd = -1;         /* UDS listener (always bound) */
static int g_tcp_fd = -1;            /* optional localhost TCP listener */
static int g_tls_fd = -1;            /* optional native-TLS listener (phase 1b) */
static int g_management_tls_fd = -1; /* dedicated required-mTLS management listener */
static char g_uds_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
static char g_bearer[256] = ""; /* configured TCP bearer (empty = none) */
static int g_rate_limit = 0;    /* TCP requests / 60s (0 = unlimited) */
int g_remote_writes = 0;        /* aimee.api.remote_writes: parsed, authorizes nothing */
static server_http_rate_state_t g_rate_state = {0, 0};
static pthread_mutex_t g_rate_lock = PTHREAD_MUTEX_INITIALIZER;
/* guards g_rate_state across conns */
static pthread_t g_thread;
static atomic_int g_running = 0;
static int g_thread_active = 0;
/* Serialize publication/teardown so start cannot reuse an fd before the old loop exits. */
static pthread_mutex_t g_listener_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
const char *server_http_default_path(void)
{
   static char path[512];
   snprintf(path, sizeof(path), "%s/aimee-http.sock", config_default_dir());
   return path;
}

/* Hot-swap the live TCP/TLS bearer without a restart. Rotation is a revoke-all
 * operation, so it atomically clears every additionally-enrolled bearer too. */
void server_http_set_bearer(const char *bearer)
{
   server_http_update_primary_bearer(g_bearer, sizeof(g_bearer), bearer, 1 /* revoke enrolled */);
}

/* Write the whole buffer. Returns the bytes written, or -1 on a write error
 * (used by the live SSE path to detect a client disconnect). Existing callers
 * ignore the return value. */
int write_all_fd(int fd, const char *buf, int len)
{
   /* Routes through the conn-io shim: over the fd's SSL if one is registered
    * (native TLS), else raw write. With nothing registered this is byte-identical
    * to the previous raw write loop, so all existing callers are unaffected. */
   return server_conn_io_write_all(fd, buf, len);
}

/* Buffered HTTP response writers (request_id_header, retrieval_event_header,
 * send_response, send_rate_limited) live in this textual include to keep this
 * file under the per-file line cap. Included here, before their first use. */

/* ── SSE streaming for /v1/chat/completions ─────────────────────────────── */

/* emit context: the connection fd, threaded through the stream handler. */
static void sse_emit(void *ctx, const char *frame_json)
{
   int fd = *(int *)ctx;
   if (!frame_json)
      return;
   write_all_fd(fd, "data: ", 6);
   write_all_fd(fd, frame_json, (int)strlen(frame_json));
   write_all_fd(fd, "\n\n", 2);
}

/* Write the SSE response headers (echoing X-Request-ID when present). */
void write_sse_headers(int fd, const char *request_id)
{
   char rid[96];
   request_id_header(rid, sizeof(rid), request_id);
   char reh[80];
   retrieval_event_header(reh, sizeof(reh));
   char head[336];
   int hlen = snprintf(head, sizeof(head),
                       "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                       "Cache-Control: no-cache\r\n%s%sConnection: close\r\n\r\n",
                       rid, reh);
   write_all_fd(fd, head, hlen);
}

/* Run a streaming request through `fn`: write the event-stream headers, let
 * the handler emit OpenAI chunk frames, then terminate with `data: [DONE]`.
 * The connection is closed by the caller. */
static void handle_stream(int fd, const char *body, server_http_stream_fn fn,
                          const char *request_id)
{
   write_sse_headers(fd, request_id);
   fn(body ? body : "", sse_emit, &fd);
   write_all_fd(fd, "data: [DONE]\n\n", 14);
}

/* Write the NDJSON-stream response headers (read-until-close; no Content-Length).
 * Used by the native chat stream, whose body is newline-delimited aimee events. */
static void write_ndjson_stream_headers(int fd, const char *request_id)
{
   char rid[96];
   request_id_header(rid, sizeof(rid), request_id);
   char head[256];
   int hlen =
       snprintf(head, sizeof(head),
                "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                "Cache-Control: no-cache\r\nX-Accel-Buffering: no\r\n%sConnection: close\r\n\r\n",
                rid);
   write_all_fd(fd, head, hlen);
}

/* POST /v1/chat/stream: native streaming chat over HTTP. Write NDJSON-stream
 * headers, then hand the connection to the async chat worker
 * (handle_chat_send_stream), which dup()s the fd and streams newline-delimited
 * aimee events (turn_start/text/thinking/turn_end/session/usage/done/error) on
 * the session pool until it closes its copy. This is the same worker the NDJSON
 * socket uses; it holds no pointer to the conn (only the dup'd fd + a cloned
 * request), so the stack conn here is safe to tear down on return. Non-blocking
 * for the single-threaded listener: close(fd) by the caller just drops its own
 * reference while the worker keeps streaming on the dup. */
static void handle_native_chat_stream(int fd, const char *body, uint32_t conn_caps,
                                      const char *request_id)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   if (!req)
   {
      send_response(fd, 400, "{\"error\":\"invalid JSON body\"}", request_id);
      return;
   }
   write_ndjson_stream_headers(fd, request_id);
   server_conn_t sc;
   memset(&sc, 0, sizeof(sc));
   sc.fd = fd;
   sc.capabilities = conn_caps;
   pthread_mutex_init(&sc.mutex, NULL);
   pthread_cond_init(&sc.can_close, NULL);
   handle_chat_send_stream(server_active_ctx(), &sc, req);
   pthread_mutex_destroy(&sc.mutex);
   pthread_cond_destroy(&sc.can_close);
   cJSON_Delete(req);
}

static int append_sse_text(char *buf, size_t n, size_t *pos, const char *s, size_t len)
{
   if (buf && *pos < n)
   {
      size_t avail = n - *pos;
      size_t copy = len < avail ? len : avail - 1;
      if (copy)
         memcpy(buf + *pos, s, copy);
      buf[*pos + copy] = '\0';
   }
   *pos += len;
   return (int)*pos;
}

int server_http_sse_event_format(const char *event, const char *data_json, char *buf, size_t n)
{
   size_t pos = 0;
   const char *p;

   if (buf && n)
      buf[0] = '\0';
   if (!data_json)
      return 0;
   if (event && event[0])
   {
      append_sse_text(buf, n, &pos, "event: ", 7);
      append_sse_text(buf, n, &pos, event, strlen(event));
      append_sse_text(buf, n, &pos, "\n", 1);
   }
   p = data_json;
   for (;;)
   {
      const char *nl = strchr(p, '\n');
      append_sse_text(buf, n, &pos, "data: ", 6);
      append_sse_text(buf, n, &pos, p, nl ? (size_t)(nl - p) : strlen(p));
      append_sse_text(buf, n, &pos, "\n", 1);
      if (!nl)
         break;
      p = nl + 1;
   }
   append_sse_text(buf, n, &pos, "\n", 1);
   return (int)pos;
}

/* Typed-event emit for the Responses API: `event: <name>\ndata: <json>\n\n`. */
static void sse_event_emit(void *ctx, const char *event, const char *data_json)
{
   int fd = *(int *)ctx;
   int need;
   char *frame;

   if (!data_json)
      return;
   need = server_http_sse_event_format(event, data_json, NULL, 0);
   frame = malloc((size_t)need + 1);
   if (!frame)
      return;
   server_http_sse_event_format(event, data_json, frame, (size_t)need + 1);
   write_all_fd(fd, frame, need);
   free(frame);
}

/* Run a streaming /v1/responses request: write event-stream headers, let the
 * handler emit typed events; the Responses protocol has no `data: [DONE]`
 * terminator (it ends with the handler's `response.completed`). */
static void handle_responses_stream(int fd, const char *body, const char *request_id)
{
   write_sse_headers(fd, request_id);
   g_responses_stream_handler(body ? body : "", sse_event_emit, &fd);
}

/* SSE for POST /v1/messages (Anthropic Messages API, stream:true). Emits the
 * Anthropic typed-event sequence (message_start … message_stop) via the same
 * `event:`/`data:` framer as Responses; unlike the OpenAI SSE path there is no
 * terminal `data: [DONE]` (the stream ends with message_stop). */
static void handle_messages_stream(int fd, const char *body, const char *request_id)
{
   write_sse_headers(fd, request_id);
   g_messages_stream_handler(body ? body : "", sse_event_emit, &fd);
}

static void handle_cli_session_stream(int fd, const char *id, const char *request_id)
{
   if (!cli_session_pty_forwarding_enabled())
   {
      send_response(fd, 404, "{\"error\":\"cli session forwarding disabled\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   cli_session_pty_stream(id, fd);
}

/* GET /v1/runs/{id}/events: subscribe to the live run record. Flush already
 * buffered events, then block-and-flush new ones as the background worker
 * produces them, until a terminal event. A periodic SSE comment heartbeat
 * doubles as a client-disconnect probe so a hangup frees the listener. Missing
 * valid op-runs return 410; arbitrary unknown ids return 404. */
static void handle_run_events(int fd, const char *id, const char *request_id)
{
   openai_run_status_t st0;
   if (!openai_runs_store_status(id, &st0))
   {
      openai_runs_missing_t why = openai_runs_store_classify_missing(id);
      if (why == OPENAI_RUNS_MISSING_INTERRUPTED)
         send_response(fd, 410,
                       "{\"error\":\"run interrupted by server replacement\","
                       "\"status\":\"interrupted\"}",
                       request_id);
      else if (why == OPENAI_RUNS_MISSING_EVICTED)
         send_response(fd, 410,
                       "{\"error\":\"run record evicted from live store\","
                       "\"status\":\"evicted\"}",
                       request_id);
      else
         send_response(fd, 404, "{\"error\":\"no such run\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   char *data = (char *)malloc(OPENAI_RUNS_EVENT_MAX + 1);
   if (!data)
      return; /* headers already sent; just drop the stream */
   char ev[64];
   size_t cursor = 0;
   for (;;)
   {
      openai_runs_wait_t w = openai_runs_store_wait(id, &cursor, 1000, ev, sizeof(ev), data,
                                                    OPENAI_RUNS_EVENT_MAX + 1);
      if (w == OPENAI_RUNS_WAIT_EVENT)
      {
         /* `event: <name>\n data: <json>\n\n` — checked writes detect a hangup. */
         if (ev[0])
         {
            if (write_all_fd(fd, "event: ", 7) < 0)
               break;
            if (write_all_fd(fd, ev, (int)strlen(ev)) < 0)
               break;
            if (write_all_fd(fd, "\n", 1) < 0)
               break;
         }
         if (write_all_fd(fd, "data: ", 6) < 0)
            break;
         if (write_all_fd(fd, data, (int)strlen(data)) < 0)
            break;
         if (write_all_fd(fd, "\n\n", 2) < 0)
            break;
         continue;
      }
      if (w == OPENAI_RUNS_WAIT_TERMINAL || w == OPENAI_RUNS_WAIT_GONE)
         break;
      /* OPENAI_RUNS_WAIT_TIMEOUT: heartbeat; a failed write means the client
       * disconnected, so stop streaming and free the slot. */
      if (write_all_fd(fd, ": keep-alive\n\n", 13) < 0)
         break;
   }
   free(data);
}

/* GET /v1/sessions/{id}/events: subscribe to a session's presence-event stream
 * (turn_started/turn_delta/turn_done/busy and routed async events). Mirrors
 * handle_run_events but over the presence ring: flush already-buffered events
 * then block-and-flush new ones, with a periodic SSE comment heartbeat that
 * doubles as a disconnect probe. The stream ends when the presence is torn
 * down (PRESENCE_WAIT_GONE) or the client hangs up (a failed write). 404 when
 * the session has no live presence. */
/* handle_session_events lives in server_http_sse.inc (textual include) to keep
 * this file under the 2000-line cap; it shares this TU's static SSE helpers. */

/* SSE event streams (handle_session_events / handle_run_events) are long-lived:
 * running them inline in handle_conn would block the single listener thread for
 * the stream's whole lifetime, starving every other /v1 connection (a presence
 * subscriber would freeze the entire HTTP surface). Offload each to a detached
 * thread on a dup'd fd — mirroring the chat.send_stream worker — so the listener
 * returns immediately. The listener's close(fd) only drops its own reference;
 * the worker streams on the dup until the client disconnects, then closes it. A
 * modest cap bounds concurrent streams against fd/thread exhaustion. */
typedef void (*sse_stream_fn)(int fd, const char *id, const char *request_id);
typedef struct
{
   int fd;
   sse_stream_fn fn;
   char id[160];
   char request_id[64];
} sse_offload_t;

static atomic_int g_sse_live = 0;
#define SSE_LIVE_DEFAULT 256
static int g_sse_max = SSE_LIVE_DEFAULT; /* configurable via aimee.api.max_event_streams */

/* Set the cap on concurrent SSE event streams. n <= 0 restores the default.
 * Called once at startup (server_http_start) before any stream is accepted. */
void server_http_set_max_event_streams(int n)
{
   g_sse_max = (n > 0) ? n : SSE_LIVE_DEFAULT;
}

static void *sse_offload_thread(void *arg)
{
   sse_offload_t *o = (sse_offload_t *)arg;
   o->fn(o->fd, o->id, o->request_id);
   close(o->fd);
   atomic_fetch_sub(&g_sse_live, 1);
   free(o);
   return NULL;
}

/* Hand an SSE stream to a detached worker on a dup'd fd. Returns 0 on success
 * (the caller must not touch fd afterward beyond the listener's own close).
 * Returns -1 if the live-stream cap is hit or resources are exhausted, in which
 * case the caller should send an error on fd and not stream. */
static int sse_offload(int fd, sse_stream_fn fn, const char *id, const char *request_id)
{
   /* SSE offload dups the fd to a detached thread; a TLS conn's SSL is owned by
    * this conn's worker and cannot be shared with the dup (and the dup has no SSL
    * registered → it would emit plaintext on the TLS socket). Refuse over TLS; the
    * caller turns this into a 503. SSE-over-TLS is phase 1c. */
   if (server_conn_io_has_ssl(fd))
      return -1;
   if (atomic_fetch_add(&g_sse_live, 1) >= g_sse_max)
   {
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   int dfd = dup(fd);
   if (dfd < 0)
   {
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   sse_offload_t *o = (sse_offload_t *)calloc(1, sizeof(*o));
   if (!o)
   {
      close(dfd);
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   o->fd = dfd;
   o->fn = fn;
   snprintf(o->id, sizeof(o->id), "%s", id ? id : "");
   snprintf(o->request_id, sizeof(o->request_id), "%s", request_id ? request_id : "");
   pthread_t t;
   if (pthread_create(&t, NULL, sse_offload_thread, o) != 0)
   {
      close(dfd);
      free(o);
      atomic_fetch_sub(&g_sse_live, 1);
      return -1;
   }
   pthread_detach(t);
   return 0;
}

/* Extract a request header value (case-insensitive name match) from the raw
 * request `buf` into out[n], trimmed of leading whitespace and the trailing
 * CR/LF. Returns 1 if found, 0 otherwise (out gets ""). */
int http_header(const char *buf, const char *name, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (!buf || !name || !out || !n)
      return 0;
   size_t nlen = strlen(name);
   for (const char *line = buf; line && *line;)
   {
      const char *eol = strstr(line, "\r\n");
      size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
      if (linelen > nlen && strncasecmp(line, name, nlen) == 0 && line[nlen] == ':')
      {
         const char *v = line + nlen + 1;
         while (*v == ' ' || *v == '\t')
            v++;
         size_t vlen = (size_t)((line + linelen) - v);
         if (vlen >= n)
            vlen = n - 1;
         memcpy(out, v, vlen);
         out[vlen] = '\0';
         return 1;
      }
      line = eol ? eol + 2 : NULL;
   }
   return 0;
}

void handle_conn(int fd, int is_tcp, int is_management)
{
   server_http_keepalive_set(0);
   server_http_gzip_set(0);
   char buf[SHTTP_READ_MAX];
   int total = 0;
   while (total < SHTTP_READ_MAX - 1)
   {
      int n = server_conn_io_read(fd, buf + total, (int)(SHTTP_READ_MAX - 1 - total));
      if (n <= 0)
         break;
      total += n;
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
         break;
   }
   buf[total] = '\0';

   /* The dedicated lane never classifies or dispatches an unterminated or
    * header-overflow request. Generic listeners retain their legacy parser. */
   if (is_management && !strstr(buf, "\r\n\r\n"))
   {
      send_response(fd, 400, "{\"error\":\"bad management request\"}", NULL);
      return;
   }

   char method[16] = {0};
   char path[512] = {0};
   if (sscanf(buf, "%15s %511s", method, path) < 2)
   {
      send_response(fd, 400, "{\"error\":\"bad request\"}", NULL);
      return;
   }

   /* Package-access proxy (UDS listener ONLY — never the TCP/TLS surface): a CONNECT
    * or absolute-form (`http://…`) request line is a sandboxed delegate's
    * package-manager fetch arriving via the in-container aimee-forwarder. Hand it to
    * the forward proxy (host-allowlist + SSRF guard + per-request audit); origin-form
    * (`/…`) request lines fall through to the /v1 API stack below. A local /v1 client
    * never sends CONNECT, so this cannot shadow the API. The caller closes `fd`. */
   if (!is_tcp && (strcmp(method, "CONNECT") == 0 || strncmp(path, "http://", 7) == 0))
   {
      /* Defense in depth beyond !is_tcp: confirm the socket really is AF_UNIX before
       * exposing the forward proxy, so a future is_tcp regression cannot open egress on
       * the public TCP/TLS listener. sandbox_pkg_proxy_serve also refuses if !is_uds. */
      struct sockaddr_storage ss;
      socklen_t sl = sizeof(ss);
      int is_uds = getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 && ss.ss_family == AF_UNIX;
      sandbox_pkg_proxy_serve(fd, is_uds, buf, NULL, "sandbox");
      return;
   }

   /* Strip any query string from `path`, but keep a pointer to it for the few
    * routes that read query params (e.g. the session-events resume cursor). */
   char *qmark = strchr(path, '?');
   const char *query = "";
   if (qmark)
   {
      *qmark = '\0';
      query = qmark + 1;
   }

   /* Resolve the request id (inbound X-Request-ID, else a generated <pid>-<seq>)
    * for response echo + access logging. */
   char request_id[64];
   {
      static atomic_ulong s_req_seq = 0;
      char inbound[64] = "";
      http_header(buf, "X-Request-ID", inbound, sizeof(inbound));
      server_http_request_id(inbound, (int)getpid(), atomic_fetch_add(&s_req_seq, 1) + 1,
                             request_id, sizeof(request_id));
   }

   /* Validate every data-plane HTTP/1.1 frame: ambiguous lengths or transfer
    * coding are dangerous even when the connection will not be reused. */
   if (!is_management && !server_http_request_framing_valid(buf, (size_t)total))
   {
      send_response(fd, 400, "{\"error\":\"invalid request framing\"}", request_id);
      return;
   }
   /* The management lane rejects malformed/ambiguous framing before peer
    * classification, so authorization results cannot become a parser oracle. */
   const char *management_header_end = is_management ? strstr(buf, "\r\n\r\n") : NULL;
   size_t management_header_len =
       management_header_end ? (size_t)(management_header_end + 4 - buf) : (size_t)total;
   if (is_management &&
       (!server_http_management_request_syntax_valid(method, path, buf, management_header_len) ||
        (server_http_management_health_route(method, path) &&
         !server_http_management_framing_valid(method, path, buf, management_header_len)) ||
        (server_http_management_action_route(method, path) &&
         !server_http_management_action_framing_valid(method, path, buf, management_header_len)) ||
        (server_http_management_read_route(method, path) &&
         !server_http_management_read_framing_valid(method, path, buf, management_header_len))))
   {
      send_response(fd, 400, "{\"error\":\"invalid management request framing\"}", request_id);
      return;
   }

   /* A verified mTLS leaf is a first-class TCP authenticator. Re-check its
    * durable roster row before the bearer gate so required-mode clients do not
    * still depend on the shared bearer. This remains authentication only: the
    * normal route/capability gates below still deny remote-write surfaces. */
   int mtls_authenticated = 0;
   int management_authenticated = 0;
   int mtls_mode = server_tls_mtls_mode();
   char rc_cn[256] = "", rc_serial[80] = "";
   int have_verified_peer = 0;
   int have_management_peer = 0;
   server_tls_peer_cert_t management_peer;
   memset(&management_peer, 0, sizeof(management_peer));
   if (server_conn_io_has_ssl(fd))
   {
      SSL *request_ssl = server_conn_io_get_ssl(fd);
      have_verified_peer =
          server_tls_peer_identity(request_ssl, rc_cn, sizeof(rc_cn), rc_serial, sizeof(rc_serial));
      have_management_peer =
          have_verified_peer && server_tls_peer_cert(request_ssl, &management_peer) == 1;
   }
   server_http_management_auth_t management_auth =
       server_http_management_auth(method, path, is_management, have_management_peer,
                                   management_peer.management_profile, management_peer.cn);
   if (management_auth == SERVER_HTTP_MANAGEMENT_DENY)
   {
      int status = server_http_management_route(method, path) ? 401 : 403;
      send_response(fd, status,
                    status == 401 ? "{\"error\":{\"message\":\"a verified management client "
                                    "certificate is required on the management listener\",\"type\":"
                                    "\"authentication_error\"}}"
                                  : "{\"error\":{\"message\":\"management transport is not "
                                    "authorized for this route\",\"type\":\"permission_error\"}}",
                    request_id);
      LOG_INFO("server.http", "%s %s -> %d (management transport) req_id=%s", method, path, status,
               request_id);
      return;
   }
   if (management_auth == SERVER_HTTP_MANAGEMENT_ALLOW)
   {
      management_authenticated = 1;
   }
   else if (have_verified_peer && rc_serial[0])
   {
      pki_cert_status_t cs = pki_cert_check(rc_serial, (long)time(NULL));
      if (cs != PKI_CERT_VALID)
      {
         LOG_INFO("server.http", "%s %s -> 403 (mtls re-check: %s serial=%s) req_id=%s", method,
                  path, pki_cert_status_str(cs), rc_serial, request_id);
         send_response(fd, 403,
                       "{\"error\":{\"message\":\"client certificate is no longer valid "
                       "(revoked, expired, or unrecognized)\",\"type\":"
                       "\"authentication_error\"}}",
                       request_id);
         return;
      }
      mtls_authenticated = 1;
      /* Presentation writes are migration-only. Once required, the durable
       * roster is still checked above but the steady-state request path does
       * not take DB1's write gate or recompute the whole roster hash. */
      if (mtls_mode == 1)
      {
         long ramp_now = (long)time(NULL);
         if (pki_mtls_note_presentation(rc_serial, ramp_now) != 0)
            LOG_WARN("server.http", "mTLS ramp presentation write failed serial=%s", rc_serial);
         else if (pki_mtls_ramp_ready(ramp_now) == 1)
         {
            SSL_CTX *prepared = server_tls_prepare_required();
            if (!prepared)
               LOG_WARN("server.http", "mTLS ramp required-context preparation failed");
            else
            {
               int advanced = pki_mtls_ramp_advance(ramp_now);
               if (advanced == 1)
                  server_tls_activate_required(prepared);
               else
               {
                  server_tls_discard_prepared(prepared);
                  if (advanced < 0)
                     LOG_WARN("server.http", "mTLS ramp durable advance failed");
               }
            }
         }
      }
   }

   /* Promotion applies to connections accepted under the old optional context
    * too: once durable state is required, a no-cert keep-alive may not retain
    * bearer fallback merely because its handshake predated the context swap. */
   mtls_mode = server_tls_mtls_mode();
   int transport_authenticated = mtls_authenticated || management_authenticated;
   if (!server_http_mtls_transport_allowed(is_tcp, mtls_mode, transport_authenticated, method,
                                           path))
   {
      send_response(fd, 401,
                    "{\"error\":{\"message\":\"a valid client certificate is required\","
                    "\"type\":\"authentication_error\"}}",
                    request_id);
      LOG_INFO("server.http", "%s %s -> 401 (mtls required) req_id=%s", method, path, request_id);
      return;
   }
   /* Per-user remote_writes (proposal §5): the process-global no longer
    * authorizes anything — the tier comes from the caller's own kb-signed
    * identity token, resolved here BEFORE capabilities are derived. Verification
    * has no side effects; the token's single-use jti is spent further down, only
    * once the request is known to be servable. UDS never reaches the gate at all
    * (server_http_resolve_write_tier is a no-op when !is_tcp), so the local
    * operator keeps full capability exactly as §7 requires. */
   server_identity_token_claims_t identity_claims;
   int identity_present = 0;
   int effective_remote_writes = server_http_resolve_write_tier(
       is_tcp, buf, method, path, request_id, &identity_claims, &identity_present);
   char first_user_principal[128] = "";
   (void)server_http_first_user_apply_cert_grant(mtls_authenticated, rc_serial,
                                                 &effective_remote_writes, first_user_principal,
                                                 sizeof(first_user_principal));
   char request_bearer[sizeof(g_bearer)];
   server_http_primary_bearer_snapshot(g_bearer, request_bearer, sizeof(request_bearer));
   uint32_t effective_caps =
       management_authenticated
           ? 0
           : server_http_effective_conn_caps(is_tcp, request_bearer, effective_remote_writes,
                                             mtls_mode, mtls_authenticated);
   effective_caps =
       server_http_enrollment_caps(effective_caps, is_tcp, mtls_authenticated,
                                   server_conn_io_has_ssl(fd), request_bearer, method, path);
   /* Establish the per-request context (#3) only after authenticating the
    * durable certificate, so downstream dispatch sees the same effective caps
    * as the outer route gate. */
   server_http_populate_request_context(fd, is_tcp, buf, request_id, method, path, effective_caps);
   if (first_user_principal[0])
      request_context_override_principal(first_user_principal);
   /* Authorize before reading the body: TCP requires either the durably valid
    * client certificate above or a valid bearer; UDS relies on permissions. */
   {
      char auth[512] = "";
      char api_key[512] = "";
      char skey[256] = "";
      int has_auth = http_header(buf, "Authorization", auth, sizeof(auth));
      int has_api_key = http_header(buf, "x-api-key", api_key, sizeof(api_key));
      int has_skey = http_header(buf, "X-Aimee-Session-Key", skey, sizeof(skey));
      /* Per-request pre-injection override: `x-aimee-preinject: 0` disables the
       * <aimee-context> envelope for this turn (set every request so it never
       * leaks across requests on a reused worker thread). */
      char preinject[16] = "";
      ingress_preinject_set_request_disabled(
          http_header(buf, "X-Aimee-Preinject", preinject, sizeof(preinject)) &&
          strcmp(preinject, "0") == 0);
      /* Auditable-correctness P1: clear any turn id left by a prior request on
       * this reused worker thread; the OpenAI-family ingress dispatch mints a
       * fresh one below when evidence emission is on. */
      ingress_preinject_set_turn_id("");
      anthropic_http_capture_request_headers(buf); /* parity: per-request anthropic-* hdrs */
      int az = transport_authenticated
                   ? 0
                   : server_http_authorize_enrolled(is_tcp, g_bearer, has_auth ? auth : NULL,
                                                    has_api_key ? api_key : NULL, has_skey);
      if (az != 0)
      {
         const char *msg = server_http_auth_error_body(az);
         send_response(fd, az, msg, request_id);
         /* The container healthcheck GETs /v1/health with no credential every 10s
          * and treats 401 as healthy (it has no bearer to present, and having one
          * in container metadata would be worse). Logging that designed probe at
          * INFO produced a 401 line every 10 seconds for the life of the server —
          * noise that buries the auth failures worth reading. Demote just that
          * shape; every other rejection still logs. */
         int health_probe = az == 401 && strcmp(method, "GET") == 0 &&
                            strcmp(path, "/v1/health") == 0 && !has_auth && !has_api_key &&
                            !has_skey;
         if (health_probe)
            LOG_DEBUG("server.http", "%s %s -> %d req_id=%s (unauthenticated health probe)", method,
                      path, az, request_id);
         else
            LOG_INFO("server.http", "%s %s -> %d req_id=%s", method, path, az, request_id);
         return;
      }
   }

   /* Per-bearer rate limit on the TCP listener (the UDS listener is local and
    * never throttled). Connections are handled concurrently, so g_rate_state is
    * mutated under g_rate_lock. */
   if (is_tcp && !management_authenticated)
   {
      pthread_mutex_lock(&g_rate_lock);
      int retry = server_http_rate_check(&g_rate_state, g_rate_limit, (long)time(NULL));
      pthread_mutex_unlock(&g_rate_lock);
      if (retry > 0)
      {
         send_rate_limited(fd, retry, request_id);
         LOG_INFO("server.http", "%s %s -> 429 req_id=%s", method, path, request_id);
         return;
      }
   }

   /* Per-route capability gate (TCP only): the route's required capabilities
    * must be a subset of the connection's effective set. UDS is same-user
    * trusted and exempt. A scoped bearer is read/query-only, so compute/write
    * routes return 403; an unscoped bearer holds CAPS_AUTHENTICATED. */
   if (is_tcp && !server_http_route_allowed_caps(is_tcp, effective_caps, method, path,
                                                 effective_remote_writes))
   {
      /* Name the remedy. The bare "beyond the presented token's scope" left a
       * caller with nowhere to go: following QUICKSTART end to end now lands here
       * on the first kb write, and nothing on screen says a write-tier grant is
       * what is missing or who issues it. The mechanism is already public
       * (QUICKSTART 1.4, docs/UPGRADING.md), so pointing at it leaks nothing. */
      send_response(fd, 403,
                    "{\"error\":{\"message\":\"this endpoint requires capabilities beyond the "
                    "presented token's scope. Over the network a bearer is read/query only "
                    "until your subject holds a write-tier grant on this server; an operator "
                    "issues one with `aimee kb grant set` (see docs/UPGRADING.md).\","
                    "\"type\":\"permission_error\"}}",
                    request_id);
      /* Count the requests the retired global would formerly have allowed, so an
       * operator can see exactly how much traffic the cutover is refusing
       * instead of inferring it from complaints. Only counts denials the old
       * global would have permitted - a request that fails for an unrelated
       * reason is not attributable to this change. */
      if (!management_authenticated &&
          server_http_retired_global_would_allow(fd, is_tcp, request_bearer, g_remote_writes,
                                                 mtls_mode, mtls_authenticated, method, path))
         server_http_note_global_ignored();
      LOG_INFO("server.http", "%s %s -> 403 (caps) req_id=%s", method, path, request_id);
      return;
   }

   /* The request has cleared every gate, so now - and only now - spend the
    * token's single-use jti. Doing it earlier would burn a token on a request
    * that was never served. The check remains binding: a replay, or a replay
    * store that cannot answer, denies here even though verification succeeded. */
   if (identity_present)
   {
      server_write_tier_outcome_t consumed = SERVER_WRITE_TIER_INVALID;
      if (server_write_tier_consume_for_request(&identity_claims, (int64_t)time(NULL), &consumed) ==
              SERVER_REMOTE_WRITES_OFF &&
          consumed != SERVER_WRITE_TIER_OK)
      {
         OPENSSL_cleanse(&identity_claims, sizeof(identity_claims));
         send_response(fd, 403,
                       "{\"error\":{\"message\":\"this identity token has already been "
                       "used\",\"type\":\"permission_error\"}}",
                       request_id);
         LOG_INFO("server.http", "%s %s -> 403 (%s) req_id=%s", method, path,
                  server_write_tier_outcome_str(consumed), request_id);
         return;
      }
   }
   OPENSSL_cleanse(&identity_claims, sizeof(identity_claims));
   char connection_header[128] = "";
   int keepalive_requested =
       server_conn_io_has_ssl(fd) && server_http_keepalive_route_eligible(path) &&
       http_header(buf, "Connection", connection_header, sizeof(connection_header)) &&
       strcasestr(connection_header, "keep-alive") != NULL &&
       strcasestr(connection_header, "close") == NULL;
   if (config_transport_server_keepalive_enabled() && keepalive_requested)
      server_http_keepalive_set(1);
   char accept_encoding[128] = "";
   int gzip_allowed =
       config_transport_thinclient_gzip_enabled() && server_http_gzip_route_eligible(path);
   server_http_gzip_set(
       gzip_allowed &&
       http_header(buf, "Accept-Encoding", accept_encoding, sizeof(accept_encoding)) &&
       strcasestr(accept_encoding, "gzip") != NULL);

   /* GET /v1/runs/{id}/events takes the SSE path (no body); other run routes
    * fall through to the buffered router below. */
   {
      static const char *RPFX = "/v1/runs/";
      if (strcmp(method, "GET") == 0 && strncmp(path, RPFX, strlen(RPFX)) == 0)
      {
         const char *rest = path + strlen(RPFX);
         const char *slash = strchr(rest, '/');
         if (slash && strcmp(slash, "/events") == 0)
         {
            char id[128];
            size_t idlen = (size_t)(slash - rest);
            if (idlen >= sizeof(id))
               idlen = sizeof(id) - 1;
            memcpy(id, rest, idlen);
            id[idlen] = '\0';
            /* Offload to a detached worker so the long-lived SSE stream does not
             * block the listener thread; fall back to a 503 if the cap is hit. */
            if (sse_offload(fd, handle_run_events, id, request_id) != 0)
               send_response(fd, 503, "{\"error\":\"too many event streams\"}", request_id);
            LOG_INFO("server.http", "GET %s -> events req_id=%s", path, request_id);
            return;
         }
      }
   }

   /* GET /v1/sessions/{id}/events takes the presence-event SSE path. */
   {
      static const char *SPFX = "/v1/sessions/";
      if (strcmp(method, "GET") == 0 && strncmp(path, SPFX, strlen(SPFX)) == 0)
      {
         const char *rest = path + strlen(SPFX);
         const char *slash = strchr(rest, '/');
         if (slash && strcmp(slash, "/events") == 0)
         {
            char id[128];
            size_t idlen = (size_t)(slash - rest);
            if (idlen >= sizeof(id))
               idlen = sizeof(id) - 1;
            memcpy(id, rest, idlen);
            id[idlen] = '\0';
            /* A resume position is threaded to handle_session_events through the
             * id slot ("<sid>?cursor=N") so a client resumes without replaying
             * the whole ring from 0. Two sources, Last-Event-ID first: an
             * EventSource auto-reconnect sends the native Last-Event-ID header
             * (most recent); a fresh remount has no header and instead passes
             * ?cursor=<persisted> in the query. Absent/0 = replay from oldest. */
            char idc[160];
            char leid[40] = "";
            const char *src = NULL;
            if (http_header(buf, "Last-Event-ID", leid, sizeof(leid)) && leid[0])
               src = leid; /* EventSource auto-reconnect (most recent) */
            else
            {
               const char *cur = strstr(query, "cursor=");
               if (cur)
                  src = cur + 7; /* explicit ?cursor= on a fresh remount */
            }
            /* Validate as a fully-numeric uint64: a bare strtoull would accept
             * "123abc" as 123. Reject trailing garbage (except '&' ending a query
             * param) and overflow; on anything malformed fall back to 0 (replay
             * from oldest). An in-range-but-wrong cursor is already safe —
             * presence_wait reads ring[cursor % RING] and waits when
             * cursor >= ev_total — but we still refuse junk input. */
            unsigned long long n = 0;
            if (src && *src)
            {
               errno = 0;
               char *end = NULL;
               unsigned long long v = strtoull(src, &end, 10);
               if (end != src && errno != ERANGE && (*end == '\0' || *end == '&'))
                  n = v;
            }
            if (n > 0)
               snprintf(idc, sizeof(idc), "%s?cursor=%llu", id, n);
            else
               snprintf(idc, sizeof(idc), "%s", id);
            /* Offload to a detached worker so the long-lived presence SSE stream
             * does not block the listener thread (a subscriber would otherwise
             * freeze the whole /v1 surface); 503 if the live-stream cap is hit. */
            if (sse_offload(fd, handle_session_events, idc, request_id) != 0)
               send_response(fd, 503, "{\"error\":\"too many event streams\"}", request_id);
            LOG_INFO("server.http", "GET %s -> presence events req_id=%s", path, request_id);
            return;
         }
      }
   }

   /* GET /v1/cli/session/{id}/stream takes the PTY-forwarding SSE path. */
   if (strcmp(method, "GET") == 0 && strncmp(path, "/v1/cli/session/", 16) == 0)
   {
      const char *slash = strchr(path + 16, '/');
      if (slash && strcmp(slash, "/stream") == 0)
      {
         char id[128];
         size_t idlen = (size_t)(slash - (path + 16));
         idlen = idlen < sizeof(id) ? idlen : sizeof(id) - 1;
         memcpy(id, path + 16, idlen);
         id[idlen] = '\0';
         if (sse_offload(fd, handle_cli_session_stream, id, request_id) != 0)
            send_response(fd, 503, "{\"error\":\"too many event streams\"}", request_id);
         return;
      }
   }
   char *body = NULL;
   int body_len = 0;
   char clbuf[32] = "";
   if (http_header(buf, "Content-Length", clbuf, sizeof(clbuf)))
   {
      errno = 0;
      char *clend = NULL;
      long declared = strtol(clbuf, &clend, 10);
      int route_limit =
          !strcmp(path, "/v1/roundtable/review") ? SHTTP_MAX_ROUNDTABLE_BODY : SHTTP_MAX_BODY;
      int invalid = errno == ERANGE || clend == clbuf || *clend != '\0' || declared < 0;
      if (invalid || declared > route_limit)
      {
         server_http_keepalive_set(0);
         send_response(fd, invalid ? 400 : 413,
                       invalid ? "{\"error\":\"invalid content length\"}"
                               : "{\"error\":\"request body exceeds route limit\"}",
                       request_id);
         return;
      }
      body_len = (int)declared;
      /* Allocate against bytes RECEIVED, not bytes claimed -- http_read_body
       * grows as data arrives, with `body_len` (already validated against the
       * route limit above) as the ceiling. */
      const char *bs = strstr(buf, "\r\n\r\n");
      int prefix_len = 0;
      if (bs)
      {
         bs += 4;
         prefix_len = (int)(buf + total - bs);
         if (prefix_len < 0)
            prefix_len = 0;
         if (prefix_len > body_len)
            prefix_len = body_len;
      }
      int already = 0;
      body = http_read_body(fd, bs, prefix_len, body_len, &already);
      if (body)
      {
         int declared_body_len = body_len;
         if (server_http_keepalive_peek() && already != declared_body_len)
         {
            server_http_keepalive_set(0);
            send_response(fd, 400, "{\"error\":\"incomplete keep-alive request body\"}",
                          request_id);
            free(body);
            return;
         }
         body_len = already;
      }
      else if (body_len > 0 && server_http_keepalive_peek())
      {
         server_http_keepalive_set(0);
         send_response(fd, 500, "{\"error\":\"oom\"}", request_id);
         return;
      }
   }

   char content_encoding[64] = "";
   if (http_header(buf, "Content-Encoding", content_encoding, sizeof(content_encoding)) &&
       content_encoding[0])
   {
      if (!gzip_allowed || strcasecmp(content_encoding, "gzip") != 0 || !body)
      {
         server_http_keepalive_set(0);
         send_response(fd, 415, "{\"error\":\"unsupported content encoding\"}", request_id);
         free(body);
         return;
      }
      unsigned char *decoded = NULL;
      size_t decoded_len = 0;
      const char *head_end = strstr(buf, "\r\n\r\n");
      size_t head_bytes = head_end ? (size_t)(head_end + 4 - buf) : (size_t)total;
      size_t decoded_cap = head_bytes < 64u * 1024u ? 64u * 1024u - head_bytes : 0;
      int gzip_rc = decoded_cap ? http_gzip_decompress(body, (size_t)body_len, decoded_cap, 50,
                                                       &decoded, &decoded_len)
                                : -2;
      free(body);
      body = (char *)decoded;
      body_len = (int)decoded_len;
      if (gzip_rc != 0)
      {
         server_http_keepalive_set(0);
         send_response(fd, gzip_rc == -2 ? 413 : 400,
                       gzip_rc == -2 ? "{\"error\":\"decompressed body limit exceeded\"}"
                                     : "{\"error\":\"malformed gzip body\"}",
                       request_id);
         return;
      }
   }

   /* Shadow-traffic mirror: fire-and-forget a copy of every completion request to
    * a configured peer aimee before we serve it, so a build under test can be
    * validated against live traffic without being deployed here. No-op unless a
    * peer is configured. The X-Aimee-Shadow header on an INBOUND request means we
    * are the peer receiving a mirror -- do not re-mirror (loop guard). Placed
    * after the body is read and before dispatch so it covers both the streaming
    * and buffered completion paths, and it never blocks or alters the real turn. */
   if (strcmp(method, "POST") == 0 && shadow_mirror_is_mirrorable_path(path))
   {
      char shadow_hdr[8] = "";
      int inbound_is_shadow = http_header(buf, "X-Aimee-Shadow", shadow_hdr, sizeof shadow_hdr);
      shadow_mirror_dispatch(path, body, body_len, inbound_is_shadow);
   }

   /* Native streaming chat over HTTP — hands off to the async chat worker, which
    * streams newline-delimited aimee events. The outer route-allowed gate (TCP)
    * already enforces CAP_CHAT via server_http_route_caps; re-check here so the
    * UDS path is explicit too. */
   if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/chat/stream") == 0)
   {
      uint32_t need = server_capability_for_method("chat.send_stream");
      uint32_t have = effective_caps;
      if ((need & ~have) != 0)
      {
         send_response(fd, 403, "{\"error\":\"forbidden: chat requires an unscoped credential\"}",
                       request_id);
         free(body);
         return;
      }
      handle_native_chat_stream(fd, body, have, request_id);
      LOG_INFO("server.http", "%s %s -> 200 (chat stream) req_id=%s", method, path, request_id);
      free(body);
      return;
   }

   /* Auditable-correctness P1: for OpenAI-family ingress endpoints, mint the
    * per-turn retrieval-event id up-front — before any response header is
    * written — when evidence emission is on. The same id is then (a) surfaced
    * to the client via the X-Aimee-Retrieval-Event response header and (b)
    * reused by ingress_preinject_build for the emitted retrieval_event. Gated
    * on the endpoint + flag so flag-off / non-ingress requests are byte-
    * identical on the wire (config is read only for these three paths, which
    * already pay a config_load inside the ingress builder). */
   if (strcmp(method, "POST") == 0 &&
       (strcmp(path, "/v1/chat/completions") == 0 || strcmp(path, "/v1/completions") == 0 ||
        strcmp(path, "/v1/responses") == 0))
   {
      if (config_kb_evidence_emit_enabled())
      {
         char tid[40];
         ingress_preinject_mint_turn_id(tid, sizeof(tid));
         ingress_preinject_set_turn_id(tid);
      }
   }

   /* Streaming completions take the SSE path (separate from the buffered unary
    * route). With no stream handler registered the request falls through to the
    * unary handler, which rejects streaming. */
   int streaming_request = strcmp(method, "POST") == 0 && openai_request_bool(body, "stream");
   if (streaming_request)
   {
      server_http_keepalive_set(0);
      server_http_gzip_set(0);
   }
   if (streaming_request)
   {
      if (strcmp(path, "/v1/chat/completions") == 0 && g_chat_stream_handler)
      {
         handle_stream(fd, body, g_chat_stream_handler, request_id);
         LOG_INFO("server.http", "%s %s -> 200 (stream) req_id=%s", method, path, request_id);
         free(body);
         return;
      }
      if (strcmp(path, "/v1/completions") == 0 && g_completion_stream_handler)
      {
         handle_stream(fd, body, g_completion_stream_handler, request_id);
         LOG_INFO("server.http", "%s %s -> 200 (stream) req_id=%s", method, path, request_id);
         free(body);
         return;
      }
      if (strcmp(path, "/v1/responses") == 0 && g_responses_stream_handler)
      {
         handle_responses_stream(fd, body, request_id);
         LOG_INFO("server.http", "%s %s -> 200 (stream) req_id=%s", method, path, request_id);
         free(body);
         return;
      }
      if (strcmp(path, "/v1/messages") == 0 && g_messages_stream_handler)
      {
         handle_messages_stream(fd, body, request_id);
         LOG_INFO("server.http", "%s %s -> 200 (stream) req_id=%s", method, path, request_id);
         free(body);
         return;
      }
   }

   char *resp = malloc(SHTTP_RESP_MAX);
   if (!resp)
   {
      send_response(fd, 500, "{\"error\":\"oom\"}", request_id);
      free(body);
      return;
   }
   /* Expose this connection's effective caps to the dispatch routes (UDS =>
    * CAPS_ALL, TCP => bearer-scoped) so loopback_rpc / server_dispatch re-check
    * per-method capability. Reset to the read-only default afterward. */
   g_rpc_conn_caps = effective_caps;
   /* WP-C.0 hop 1 of 3: capture the attested vault identity (kernel UDS peer uid,
    * or a root-UDS-gated webuser assertion) into thread-locals, live until
    * loopback_rpc copies it into the synthesized conn. Cleared after the route so
    * a reused worker thread cannot leak it into the next request. */
   server_http_identity_capture(fd, is_tcp, buf);
   if (first_user_principal[0])
      server_http_identity_override_principal(first_user_principal);
   server_http_identity_set_query(query); /* cleared by server_http_identity_clear */
   int status = server_http_route(method, path, body, body_len, resp, SHTTP_RESP_MAX);
   g_rpc_conn_caps = CAPS_READ_ONLY;
   server_http_identity_clear();
   send_response(fd, status, resp, request_id);
   server_http_log_access(method, path, status, request_id);
   free(resp);
   free(body);
}
/* Per-connection worker: each accepted connection is handled on its own
 * detached thread, so a slow request (e.g. a synchronous chat completion) or an
 * SSE stream cannot block the accept loop, and independent /v1 requests run
 * concurrently. Safe because the underlying NDJSON dispatch (loopback_rpc ->
 * server_dispatch) and the chat/model stack are already concurrency-safe (the
 * socket server runs them concurrently), and the HTTP front-end's per-request
 * state is thread-local (g_rpc_conn_caps), atomic (request-id seq), or locked
 * (g_rate_state). The worker gets a large stack to match the deep dispatch call
 * chains (same reason the listener stack is 32 MB). A live-connection cap bounds
 * thread/fd use; over the cap the connection is handled inline by the accept
 * thread (degrades to serial under overload, never dropped). */
atomic_int g_conn_live = 0;
/* Keep management health traffic from exhausting data workers, and vice
 * versa. Both pools retain the same 32 MiB per-worker stack bound. */
atomic_int g_management_conn_live = 0;

/* Single accept loop over both listeners: poll the UDS (and the TCP fd when
 * bound), accept whichever is ready, and hand each connection to a per-
 * connection worker thread (conn_offload) so the accept loop never blocks. */
static void *listener_thread(void *arg)
{
   (void)arg;
   while (atomic_load(&g_running))
   {
      struct pollfd pfds[4];
      int n = 0;
      int uds_idx = -1, tcp_idx = -1, tls_idx = -1, management_idx = -1;
      if (g_listen_fd >= 0)
      {
         pfds[n].fd = g_listen_fd;
         pfds[n].events = POLLIN;
         uds_idx = n++;
      }
      if (g_tcp_fd >= 0)
      {
         pfds[n].fd = g_tcp_fd;
         pfds[n].events = POLLIN;
         tcp_idx = n++;
      }
      if (g_tls_fd >= 0)
      {
         pfds[n].fd = g_tls_fd;
         pfds[n].events = POLLIN;
         tls_idx = n++;
      }
      if (g_management_tls_fd >= 0)
      {
         pfds[n].fd = g_management_tls_fd;
         pfds[n].events = POLLIN;
         management_idx = n++;
      }
      if (n == 0)
         break;

      int pr = poll(pfds, (nfds_t)n, 1000);
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (pr == 0)
         continue; /* timeout — re-check g_running */

      if (uds_idx >= 0 && (pfds[uds_idx].revents & POLLIN))
      {
         int fd = server_conn_accept(pfds[uds_idx].fd);
         if (fd >= 0 && !conn_offload(fd, 0, 0, 0))
         {
            handle_conn(fd, 0, 0);
            close(fd);
         }
      }
      if (tcp_idx >= 0 && (pfds[tcp_idx].revents & POLLIN))
      {
         int fd = server_conn_accept(pfds[tcp_idx].fd);
         if (fd >= 0 && !conn_offload(fd, 1, 0, 0))
         {
            handle_conn(fd, 1, 0);
            close(fd);
         }
      }
      if (tls_idx >= 0 && (pfds[tls_idx].revents & POLLIN))
      {
         int fd = server_conn_accept(pfds[tls_idx].fd);
         /* TLS conns must run in a worker (the handshake + SSL live there); if the
          * conn cap is hit we drop rather than handle inline (no SSL here). */
         if (fd >= 0 && !conn_offload(fd, 1, 1, 0))
            close(fd);
      }
      if (management_idx >= 0 && (pfds[management_idx].revents & POLLIN))
      {
         int fd = server_conn_accept(pfds[management_idx].fd);
         /* Like data TLS, management TLS must never handshake on the accept
          * thread. The cap is shared so a second listener cannot double it. */
         if (fd >= 0 && !conn_offload(fd, 1, 1, 1))
            close(fd);
      }
   }
   return NULL;
}

/* Bind a TCP /v1 listener (fd, or -1 logged; the UDS listener carries on). |allow_external|
 * gates a 0.0.0.0 bind: 1 ONLY for the TLS listener. The plaintext listener passes 0 and is
 * ALWAYS loopback-bound, so credentials never cross the wire in cleartext. */
static int tcp_listen(int tcp_port, const char *bearer_token, int allow_external)
{
   if (tcp_port <= 0)
      return -1;
   if (!bearer_token || !bearer_token[0])
   {
      LOG_WARN("server.http", "aimee.api port=%d set but no bearer_token; refusing to bind TCP",
               tcp_port);
      return -1;
   }
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   int yes = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
   /* AIMEE_SERVER_HTTP_BIND requests 0.0.0.0 — honoured for TLS only (plaintext stays loopback). */
   const char *bind_all = getenv("AIMEE_SERVER_HTTP_BIND");
   int want_external = (bind_all && bind_all[0]) ? 1 : 0;
   if (want_external && !allow_external)
      LOG_ERROR("server.http",
                "AIMEE_SERVER_HTTP_BIND ignored for plaintext /v1 on port %d: a non-loopback "
                "plaintext bind would expose credentials in cleartext. Set aimee.api.tls_port "
                "for remote access; binding 127.0.0.1 only.",
                tcp_port);
   in_addr_t bind_addr = server_http_resolve_bind_addr(want_external, allow_external);
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons((uint16_t)tcp_port);
   addr.sin_addr.s_addr = htonl(bind_addr);
   if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, SHTTP_BACKLOG) < 0)
   {
      LOG_WARN("server.http", "failed to bind TCP /v1 listener on %s:%d",
               bind_addr == INADDR_ANY ? "0.0.0.0" : "127.0.0.1", tcp_port);
      close(fd);
      return -1;
   }
   return fd;
}

static int management_tcp_listen(const server_http_management_config_t *config)
{
   if (!config || !config->enabled || config->port < 1 || config->port > UINT16_MAX)
      return -1;
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   int yes = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons((uint16_t)config->port);
   addr.sin_addr.s_addr = config->bind_addr;
   if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, SHTTP_BACKLOG) < 0)
   {
      close(fd);
      return -1;
   }
   return fd;
}

static void close_listener_fd(int *fd)
{
   if (fd && *fd >= 0)
   {
      shutdown(*fd, SHUT_RDWR);
      close(*fd);
      *fd = -1;
   }
}

int server_http_start(const char *uds_path, int tcp_port, int tls_port, const char *bearer_token,
                      int rate_limit_per_min, int remote_writes)
{
   pthread_mutex_lock(&g_listener_lifecycle_lock);
   if (atomic_load(&g_running) || g_thread_active || g_listen_fd >= 0)
   {
      pthread_mutex_unlock(&g_listener_lifecycle_lock);
      return -1;
   }
   server_http_management_config_t management;
   if (server_http_management_config_from_env(&management) != 0)
   {
      LOG_ERROR("server.http", "invalid or partial dedicated management listener configuration");
      pthread_mutex_unlock(&g_listener_lifecycle_lock);
      return SERVER_HTTP_START_MGMT_FATAL;
   }
   char management_tls_key[4096] = "";
   int management_tls_ok = !management.enabled;
   if (management.enabled && server_http_management_checkpoint_files_valid(&management) &&
       runtime_secret_get(management.key, management_tls_key, sizeof(management_tls_key)) &&
       server_tls_management_init_vault(management.cert, management_tls_key,
                                        management.client_ca) == 0)
      management_tls_ok = 1;
   runtime_secret_wipe(management_tls_key, sizeof(management_tls_key));
   if (!management_tls_ok)
   {
      server_http_management_set_error("management TLS certificate/key/CA");
      LOG_ERROR("server.http", "dedicated management TLS configuration is not loadable");
      pthread_mutex_unlock(&g_listener_lifecycle_lock);
      return SERVER_HTTP_START_MGMT_FATAL;
   }
   int listener_failure = management.enabled ? SERVER_HTTP_START_MGMT_FATAL : -1;
   if (!uds_path || !uds_path[0])
      uds_path = server_http_default_path();

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", uds_path);

   unlink(uds_path);
   g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (g_listen_fd < 0)
   {
      pthread_mutex_unlock(&g_listener_lifecycle_lock);
      return listener_failure;
   }
   if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(g_listen_fd, SHTTP_BACKLOG) < 0)
   {
      close(g_listen_fd);
      g_listen_fd = -1;
      pthread_mutex_unlock(&g_listener_lifecycle_lock);
      return listener_failure;
   }
   snprintf(g_uds_path, sizeof(g_uds_path), "%s", addr.sun_path);

   /* Optional localhost TCP listener for OpenAI-style external tools. */
   /* Startup preserves the enrolled set published from config before this call. */
   server_http_update_primary_bearer(g_bearer, sizeof(g_bearer), bearer_token,
                                     0 /* preserve enrolled */);
   g_rate_limit = rate_limit_per_min > 0 ? rate_limit_per_min : 0;
   g_remote_writes = remote_writes;
   /* aimee.api.remote_writes is still parsed so an existing config file loads,
    * but it no longer authorizes anything: /v1 write authority now comes from
    * the caller's kb-signed identity token. Say so once, loudly, at startup -
    * an operator who upgraded with this set to data/full and did not notice
    * would otherwise conclude the release simply broke writes. */
   if (g_remote_writes != SERVER_REMOTE_WRITES_OFF)
      LOG_WARN("server.http",
               "aimee.api.remote_writes is set but no longer authorizes writes; per-user grants "
               "replace it (see docs/UPGRADING.md 0.3.0). Requests it would formerly have allowed "
               "are counted as remote_writes.global_ignored");
   switch (server_write_tier_config_state())
   {
   case SERVER_WRITE_TIER_CONFIG_NO_TEAM:
      LOG_ERROR("server.http",
                "AIMEE_SERVER_TEAM_ID is unset or invalid: KB-issued write tokens are denied "
                "with no_team_configured; a verified certificate-bound first owner is unaffected");
      break;
   case SERVER_WRITE_TIER_CONFIG_NO_SERVER_ID:
      LOG_ERROR("server.http",
                "AIMEE_SERVER_ID is unset: KB-issued write tokens are denied with invalid; "
                "a verified certificate-bound first owner is unaffected");
      break;
   case SERVER_WRITE_TIER_CONFIG_NO_TRUST_BUNDLE:
   {
      /* Name the path when one was supplied: the shipped standalone compose
       * defaults it to a conventional location nothing mounts, so "is unset" was
       * the one thing this could not be. */
      const char *bundle = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
      if (bundle && bundle[0])
         LOG_ERROR("server.http",
                   "AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE is set to '%s' but that file is not "
                   "readable: KB-issued write tokens are denied with invalid; a verified "
                   "certificate-bound first owner is unaffected",
                   bundle);
      else
         LOG_ERROR("server.http",
                   "AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE is unset: KB-issued write tokens are "
                   "denied with invalid; a verified certificate-bound first owner is unaffected");
      break;
   }
   case SERVER_WRITE_TIER_CONFIG_READY:
      break;
   }
   g_rate_state.window_start = 0;
   g_rate_state.count = 0;
   g_tcp_fd = tcp_listen(tcp_port, bearer_token, 0 /* plaintext: loopback only */);

   /* Optional native-TLS listener (phase 1b). A TLS+bearer connection is the
    * vault's attested write path. */
   if (tls_port > 0)
   {
      if (server_tls_init_default() == 0)
         g_tls_fd = tcp_listen(tls_port, bearer_token, 1 /* TLS: may bind 0.0.0.0 */);
      else
         /* This is the vault's attested write path — make a misconfigured cert/key
          * loud (the UDS listener still comes up; the operator must fix the cert). */
         LOG_ERROR("server.http", "tls_port=%d set but TLS cert/key not loadable; TLS DISABLED",
                   tls_port);
   }

   if (management.enabled)
   {
      g_management_tls_fd = management_tcp_listen(&management);
      if (g_management_tls_fd < 0)
      {
         server_http_management_set_error("management listener bind");
         LOG_ERROR("server.http", "failed to bind dedicated management listener on %s:%d",
                   management.bind, management.port);
         close_listener_fd(&g_tls_fd);
         close_listener_fd(&g_tcp_fd);
         close_listener_fd(&g_listen_fd);
         unlink(uds_path);
         pthread_mutex_unlock(&g_listener_lifecycle_lock);
         return SERVER_HTTP_START_MGMT_FATAL;
      }
      if (server_mgmt_checkpoint_client_start(&management) != 0)
      {
         server_http_management_set_error("management checkpoint client");
         close_listener_fd(&g_management_tls_fd);
         close_listener_fd(&g_tls_fd);
         close_listener_fd(&g_tcp_fd);
         close_listener_fd(&g_listen_fd);
         unlink(uds_path);
         pthread_mutex_unlock(&g_listener_lifecycle_lock);
         return SERVER_HTTP_START_MGMT_FATAL;
      }
   }

   server_http_management_actions_start();
   atomic_store(&g_running, 1);
   /* The listener thread runs dispatch-backed /v1 routes inline (rh_dispatch_op
    * -> loopback_rpc -> server_dispatch -> handler), whose call chains carry
    * large on-stack frames. The glibc default thread stack (~8 MB, NOT widened
    * by `ulimit -s unlimited`) overflows on a deep route (e.g. workspace.list)
    * and SIGSEGVs *only this thread* — the process survives but stops serving
    * /v1. Give it a generous explicit stack, mirroring the compute pool. */
   pthread_attr_t lattr;
   pthread_attr_t *lattr_p = NULL;
   if (pthread_attr_init(&lattr) == 0)
   {
      if (pthread_attr_setstacksize(&lattr, (size_t)32 * 1024 * 1024) == 0)
         lattr_p = &lattr;
   }
   int prc = pthread_create(&g_thread, lattr_p, listener_thread, NULL);
   if (lattr_p)
      pthread_attr_destroy(lattr_p);
   if (prc != 0)
   {
      server_http_management_actions_stop_and_wait();
      if (management.enabled)
         server_http_management_set_error("management listener thread");
      atomic_store(&g_running, 0);
      close(g_listen_fd);
      g_listen_fd = -1;
      if (g_tcp_fd >= 0)
      {
         close(g_tcp_fd);
         g_tcp_fd = -1;
      }
      close_listener_fd(&g_tls_fd);
      close_listener_fd(&g_management_tls_fd);
      server_mgmt_checkpoint_client_stop();
      unlink(uds_path);
      pthread_mutex_unlock(&g_listener_lifecycle_lock);
      return listener_failure;
   }
   g_thread_active = 1;
   /* The TLS listener (when up) logs its own "native TLS enabled" line from server_tls. */
   if (g_tcp_fd >= 0)
      LOG_INFO("server.http", "HTTP /v1 on %s and 127.0.0.1:%d (bearer, loopback only)", uds_path,
               tcp_port);
   else
      LOG_INFO("server.http", "HTTP /v1 on %s", uds_path);
   if (g_management_tls_fd >= 0)
      LOG_INFO("server.http", "dedicated management mTLS on %s:%d", management.bind,
               management.port);
   pthread_mutex_unlock(&g_listener_lifecycle_lock);
   return 0;
}

void server_http_stop(void)
{
   pthread_mutex_lock(&g_listener_lifecycle_lock);
   atomic_store(&g_running, 0);
   /* Reject the final dispatch seam of every action that has not already
    * entered the handler, then join the complete request lifetime before
    * tearing down its checkpoint-client credentials. */
   server_http_management_actions_shutdown_begin();
   /* Wake poll/accept before closing, so another thread cannot reuse its snapshotted fds. */
   if (g_listen_fd >= 0)
      shutdown(g_listen_fd, SHUT_RDWR);
   if (g_tcp_fd >= 0)
      shutdown(g_tcp_fd, SHUT_RDWR);
   if (g_tls_fd >= 0)
      shutdown(g_tls_fd, SHUT_RDWR);
   if (g_management_tls_fd >= 0)
      shutdown(g_management_tls_fd, SHUT_RDWR);
   if (g_thread_active)
   {
      pthread_join(g_thread, NULL);
      g_thread_active = 0;
   }
   server_http_management_actions_stop_and_wait();
   close_listener_fd(&g_listen_fd);
   close_listener_fd(&g_tcp_fd);
   close_listener_fd(&g_tls_fd);
   close_listener_fd(&g_management_tls_fd);
   if (g_uds_path[0])
      unlink(g_uds_path);
   g_uds_path[0] = '\0';
   server_mgmt_checkpoint_client_stop();
   pthread_mutex_unlock(&g_listener_lifecycle_lock);
}
