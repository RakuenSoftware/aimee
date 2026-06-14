/* server_http.c: aimee-server inbound HTTP-over-UDS /v1 API (see server_http.h).
 *
 * Hand-rolled HTTP/1.1 server on a dedicated Unix socket, mirroring the
 * aimee-kb HTTP server. First resource: /v1/personas. */
/* _GNU_SOURCE: struct ucred / SO_PEERCRED peer-credential capture is a GNU
 * extension; declare it before any include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server_http.h"
#include "server.h" /* CAP_* / CAPS_* capability bits, server_capability_for_method */
#include "workspace_runner_registry.h" /* ws_runner_registry_poll/_respond for the /v1 reverse channel */
#include "forge_credentials.h"         /* forge_cred_install for the /v1 token-install route */
#include <time.h>
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup */
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

#define SHTTP_READ_MAX 8192
#define SHTTP_RESP_MAX (256 * 1024)
/* Max request body. The OpenAI/Codex Responses surface sends large bodies — a
 * Codex turn carries ~20KB instructions + ~18 tool schemas + the full
 * conversation/tool-call history (175KB+ and growing), so the cap must be well
 * above the legacy 64KB or large requests are truncated and misparsed. */
#define SHTTP_MAX_BODY (4 * 1024 * 1024)
#define SHTTP_BACKLOG  16

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

static int emit(char *resp, int cap, cJSON *obj)
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

static int err_json(char *resp, int cap, int status, const char *msg)
{
   snprintf(resp, (size_t)cap, "{\"error\":\"%s\"}", msg ? msg : "error");
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
   if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0 && auth_header[7])
      authorized = server_ct_equal(auth_header + 7, bearer_cfg);
   if (api_key_header && api_key_header[0])
      authorized |= server_ct_equal(api_key_header, bearer_cfg);
   if (!authorized)
      return 401;
   return 0;
}

#define SHTTP_RATE_WINDOW_SECS 60

int server_http_rate_check(server_http_rate_state_t *st, int limit_per_min, long now)
{
   if (!st || limit_per_min <= 0)
      return 0; /* limiting disabled */
   if (now - st->window_start >= SHTTP_RATE_WINDOW_SECS || now < st->window_start)
   {
      st->window_start = now;
      st->count = 0;
   }
   if (st->count < limit_per_min)
   {
      st->count++;
      return 0;
   }
   int retry = (int)(SHTTP_RATE_WINDOW_SECS - (now - st->window_start));
   return retry > 0 ? retry : 1;
}

void server_http_request_id(const char *provided, int pid, unsigned long seq, char *buf, size_t n)
{
   if (!buf || n == 0)
      return;
   if (provided && provided[0])
      snprintf(buf, n, "%s", provided);
   else
      snprintf(buf, n, "%d-%lu", pid, seq);
}

/* The declarative /v1 route registry (server_http_routes.inc, included below)
 * is the single source of truth for dispatch, per-route capabilities, and the
 * OpenAPI path inventory. server_http_route_caps and server_http_route are thin
 * public entry points over it; route matching lives in route_match(). */
static uint32_t v1_route_caps_lookup(const char *method, const char *path);
static int v1_route_dispatch(const char *method, const char *path, const char *body, int body_len,
                             char *resp, int resp_cap);
/* 1 if the route is a data-plane write. At the default remote_writes=off these
 * routes are local-UDS-only; remote_writes=data/full can expose them over TCP
 * after the per-route capability check. */
static int v1_route_is_local_only(const char *method, const char *path);

/* Capability bitmask a /v1 route requires (route_caps subset of conn_caps gates
 * the request in handle_conn). 0 = public, or an unrecognized route (which then
 * 404s in the router). Pure — no socket, no globals. */
uint32_t server_http_route_caps(const char *method, const char *path)
{
   return v1_route_caps_lookup(method, path);
}

/* Public accessor for the data-write classification (test + introspection).
 * Historical name retained for ABI compatibility; see
 * v1_route_is_local_only in server_http_routes.inc. */
int server_http_route_is_local_only(const char *method, const char *path)
{
   return v1_route_is_local_only(method, path);
}

uint32_t server_http_conn_caps(int is_tcp, const char *bearer, int remote_writes)
{
   if (!is_tcp)
      return CAPS_ALL; /* UDS: same-user, filesystem-attested */
   if (bearer && strncmp(bearer, "scope:", 6) == 0)
      return CAPS_READ_ONLY & ~(uint32_t)CAP_CHAT; /* scoped: query-only, no compute */
   /* Unscoped TCP bearer. "full" makes it fully trusted (CAPS_ALL), which also
    * permits the delegate/tool methods over /v1 (gated on == CAPS_ALL); "off"/"data"
    * keep CAPS_AUTHENTICATED (write caps present, but mutating routes are gated
    * separately in server_http_route_allowed). */
   if (remote_writes >= SERVER_REMOTE_WRITES_FULL)
      return CAPS_ALL;
   return CAPS_AUTHENTICATED;
}

/* Routes deliberately reachable over the TCP listener regardless of
 * aimee.api.remote_writes (still capability-gated): the detached-workspace
 * reverse channel and registry mutations (workspace-resource-plane). The serving
 * client IS the fs/exec authority and must drive these from another host. */
static int v1_route_tcp_exempt(const char *method, const char *path)
{
   if (!method || !path)
      return 0;
   if (strcmp(path, "/v1/runner/poll") == 0 || strcmp(path, "/v1/runner/respond") == 0)
      return 1;
   if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/workspaces") == 0) /* workspace.add */
      return 1;
   if (strcmp(method, "DELETE") == 0 && strncmp(path, "/v1/workspaces/", 15) == 0) /* .remove */
      return 1;
   return 0;
}

int server_http_route_allowed(int is_tcp, const char *bearer, const char *method, const char *path,
                              int remote_writes)
{
   /* Over the TCP listener, a route that needs any capability beyond the read set
    * (CAPS_READ_ONLY) is "privileged" and denied unless the operator opts in via
    * aimee.api.remote_writes, so a leaked/shared bearer cannot mutate or execute
    * remotely at the default. Two tiers: data-plane writes (memory, work, rules,
    * skill, ... — v1_route_is_local_only / g_v1_write_ops) need
    * remote_writes>=data; everything else privileged (delegate, cron, agent,
    * provider, api, worktree, session admin, ...) is exec/control and needs
    * remote_writes==full. The detached-workspace plane (runner + workspace
    * add/remove) is exempt — designed to be driven by a remote fs authority. UDS
    * is the same-user trusted peer and bypasses all of this. */
   if (is_tcp && !v1_route_tcp_exempt(method, path))
   {
      int data_write = v1_route_is_local_only(method, path); /* g_v1_write_ops */
      uint32_t rc = server_http_route_caps(method, path);
      int privileged = (rc & ~(uint32_t)CAPS_READ_ONLY) != 0; /* needs a non-read cap */
      if (data_write || privileged)
      {
         /* data-plane writes open at "data"; everything else privileged (exec/
          * control) needs "full". A data-write keeps the lower bar even if its
          * cap is weak. */
         int need_tier = data_write ? SERVER_REMOTE_WRITES_DATA : SERVER_REMOTE_WRITES_FULL;
         if (remote_writes < need_tier)
            return 0;
      }
   }
   uint32_t need = server_http_route_caps(method, path);
   uint32_t have = server_http_conn_caps(is_tcp, bearer, remote_writes);
   return (need & ~have) == 0;
}

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
                 "enable the loopback listener in ~/.config/aimee/aimee.yaml:\n"
                 "  aimee:\n"
                 "    api:\n"
                 "      http_port: 8910\n"
                 "      bearer_token: \"<generated-secret>\"\n"
                 "      rate_limit_per_min: 60\n"
                 "Then re-run `aimee api status` for ready-to-paste provider snippets.\n");
      buf[n - 1] = '\0';
      return;
   }

   SAR_APPEND("  listener:      enabled on http://127.0.0.1:%d/v1\n", http_port);
   SAR_APPEND("  bearer token:  %s\n", bearer_configured
                                           ? "configured"
                                           : "NOT configured — set aimee.api.bearer_token "
                                             "(the listener refuses to bind without it)");
   if (rate_limit_per_min > 0)
      SAR_APPEND("  rate limit:    %d req/min\n", rate_limit_per_min);
   else
      SAR_APPEND("  rate limit:    unlimited\n");

   SAR_APPEND("\nVS Code model-provider setup (base URL + bearer key + model `aimee`):\n");
   SAR_APPEND(
       "  Continue / Cline / Roo Code:                 http://127.0.0.1:%d/v1   model aimee\n",
       http_port);
   SAR_APPEND(
       "  Copilot \"Manage Models\" (OpenAI-compatible): http://127.0.0.1:%d/v1   model aimee\n",
       http_port);
   SAR_APPEND("\nUse a project:-scoped bearer (scope:project:<id>:<secret>) so the editor can\n"
              "read and chat but cannot perform admin mutations.\n");
   buf[n - 1] = '\0';
#undef SAR_APPEND
}

/* Persona + role-template /v1 route handlers (kept in a sibling .inc for size). */
#include "server_http_config_routes.inc"

/* The session's active persona (set via POST below), falling back to the durable
 * default when the session has none — so a reconnecting client (e.g. webchat)
 * can render the current selection. */
static int route_session_persona_get(const char *session_id, char *resp, int cap)
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

static int route_session_persona_set(const char *session_id, const char *body, char *resp, int cap)
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

static int route_session_primary_get(const char *session_id, char *resp, int cap)
{
   if (!session_id || !session_id[0])
      return err_json(resp, cap, 400, "missing session id");
   char name[MAX_AGENT_NAME] = "";
   session_primary_get(session_id, name, sizeof(name));
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "agent", name);
   return emit(resp, cap, o);
}

static int route_session_primary_set(const char *session_id, const char *body, char *resp, int cap)
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
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || !agent_find(&acfg, name))
      return err_json(resp, cap, 404, "no such agent");

   session_primary_set(session_id, name);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "agent", name);
   return emit(resp, cap, o);
}

static int route_session_primary_clear(const char *session_id, char *resp, int cap)
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

static int route_sessions_list(char *resp, int cap)
{
   /* presence_list_json always writes valid JSON (at least "[]"). */
   presence_list_json(NULL, resp, (size_t)cap);
   return 200;
}

static int route_session_attach(const char *session_id, const char *body, char *resp, int cap)
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

static int route_session_detach(const char *session_id, const char *body, char *resp, int cap)
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

static int route_health(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"status\":\"ok\",\"service\":\"aimee-server\"}");
   return 200;
}

static int route_version(char *resp, int cap)
{
   snprintf(resp, (size_t)cap, "{\"version\":\"%s\",\"service\":\"aimee-server\"}", AIMEE_VERSION);
   return 200;
}

static int route_capabilities(char *resp, int cap)
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

static int route_models(char *resp, int cap)
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

static server_http_completion_fn g_chat_handler = NULL;
static server_http_completion_fn g_completion_handler = NULL;
static server_http_completion_fn g_embeddings_handler = NULL;
static server_http_completion_fn g_responses_handler = NULL;
static server_http_stream_fn g_chat_stream_handler = NULL;
static server_http_stream_fn g_completion_stream_handler = NULL;
static server_http_responses_stream_fn g_responses_stream_handler = NULL;
static server_http_completion_fn g_messages_handler = NULL;
static server_http_responses_stream_fn g_messages_stream_handler = NULL;
static server_http_completion_fn g_count_tokens_handler = NULL;

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
static server_http_json_provider g_rules_provider = NULL;
static server_http_json_provider g_dashboard_memory_provider = NULL;
static server_http_json_provider g_kb_status_provider = NULL;
static server_http_json_provider g_agents_provider = NULL;
static server_http_json_provider g_roadmap_provider = NULL;
static server_http_json_provider g_curiosity_provider = NULL;
static server_http_json_provider g_notes_list_provider = NULL;
static server_http_json_provider g_dashboard_reminders_provider = NULL;
static server_http_completion_fn g_kb_search_handler = NULL;
static server_http_completion_fn g_memory_recall_handler = NULL;
static server_http_completion_fn g_notes_search_handler = NULL;
static server_http_completion_fn g_runs_handler = NULL;

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

/* GET /v1/runs/{id}: return the run record (404 when unknown). The snapshot
 * reflects live status transitions (queued -> in_progress -> terminal) as the
 * background worker publishes them. */
static int route_runs_get(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err_json(resp, cap, 400, "missing run id");
   if (!openai_runs_store_get(id, resp, (size_t)cap))
      return err_json(resp, cap, 404, "no such run");
   return 200;
}

/* POST /v1/runs/{id}/stop: request cancellation of an in-flight run. Sets the
 * cancel flag (a no-op once the run is already terminal); the background worker
 * observes it at its next step boundary and finalizes the run as "cancelled".
 * Returns the current run snapshot, or 404 when unknown. */
static int route_runs_stop(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err_json(resp, cap, 400, "missing run id");
   openai_run_status_t st;
   if (!openai_runs_store_status(id, &st))
      return err_json(resp, cap, 404, "no such run");
   openai_runs_store_request_cancel(id);
   if (!openai_runs_store_get(id, resp, (size_t)cap))
      return err_json(resp, cap, 404, "no such run");
   return 200;
}

/* Dispatch a native POST body to its handler; 503 (generic JSON error) when no
 * handler is wired in (e.g. unit tests, or kb_client not linked). */
static int route_native_post(server_http_completion_fn fn, const char *body, char *resp, int cap,
                             const char *unavailable_msg)
{
   if (!fn)
      return err_json(resp, cap, 503, unavailable_msg);
   return fn(body ? body : "", resp, cap);
}

/* GET a native resource whose provider returns a heap JSON body (emitted +
 * freed here). 503 when unwired, 502 when the backend (aimee-kb) is
 * unreachable. `what` names the resource for the error messages. */
static int route_json_provider(server_http_json_provider fn, char *resp, int cap, const char *what)
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
static int route_completion(server_http_completion_fn fn, const char *body, char *resp, int cap)
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
static _Thread_local uint32_t g_rpc_conn_caps = CAPS_READ_ONLY;

/* Dispatch an NDJSON {method,params} body through server_dispatch() over a
 * socketpair loopback and capture the response into resp. The write end is
 * non-blocking so an oversize response can never hang the server thread; if the
 * response is truncated it fails to parse and we return an error rather than a
 * partial body. Returns an HTTP status; resp holds the JSON response body. */
static int loopback_rpc(const char *body, int body_len, char *resp, int resp_cap,
                        uint32_t conn_caps)
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
   shutdown(sp[1], SHUT_WR);

   size_t total = 0;
   for (;;)
   {
      ssize_t n = read(sp[0], resp + total, (size_t)resp_cap - 1 - total);
      if (n <= 0)
         break;
      total += (size_t)n;
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

#include "server_http_routes.inc"

int server_http_route(const char *method, const char *path, const char *body, int body_len,
                      char *resp, int resp_cap)
{
   return v1_route_dispatch(method, path, body, body_len, resp, resp_cap);
}

/* ── socket listener ────────────────────────────────────────────────────── */

static int g_listen_fd = -1;    /* UDS listener (always bound) */
static int g_tcp_fd = -1;       /* optional localhost TCP listener */
static char g_bearer[256] = ""; /* configured TCP bearer (empty = none) */
static int g_rate_limit = 0;    /* TCP requests / 60s (0 = unlimited) */
static int g_remote_writes = 0; /* aimee.api.remote_writes: SERVER_REMOTE_WRITES_* */
static server_http_rate_state_t g_rate_state = {0, 0};
static pthread_mutex_t g_rate_lock =
    PTHREAD_MUTEX_INITIALIZER; /* guards g_rate_state across conns */
static pthread_t g_thread;
static volatile int g_running = 0;

const char *server_http_default_path(void)
{
   static char path[512];
   snprintf(path, sizeof(path), "%s/aimee-http.sock", config_default_dir());
   return path;
}

/* Write the whole buffer. Returns the bytes written, or -1 on a write error
 * (used by the live SSE path to detect a client disconnect). Existing callers
 * ignore the return value. */
static int write_all_fd(int fd, const char *buf, int len)
{
   int off = 0;
   while (off < len)
   {
      int n = (int)write(fd, buf + off, (size_t)(len - off));
      if (n <= 0)
         return -1;
      off += n;
   }
   return off;
}

/* Buffered HTTP response writers (request_id_header, retrieval_event_header,
 * send_response, send_rate_limited) live in this textual include to keep this
 * file under the per-file line cap. Included here, before their first use. */
#include "server_http_response.inc"

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
static void write_sse_headers(int fd, const char *request_id)
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

/* GET /v1/runs/{id}/events: subscribe to the live run record. Flush already
 * buffered events, then block-and-flush new ones as the background worker
 * produces them, until a terminal event. A periodic SSE comment heartbeat
 * doubles as a client-disconnect probe so a hangup frees the listener. 404
 * (buffered) when the run is unknown. */
static void handle_run_events(int fd, const char *id, const char *request_id)
{
   openai_run_status_t st0;
   if (!openai_runs_store_status(id, &st0))
   {
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
static void handle_session_events(int fd, const char *id, const char *request_id)
{
   char probe[80];
   if (presence_session_json(id, probe, sizeof(probe)) == 0)
   {
      send_response(fd, 404, "{\"error\":\"no such session\"}", request_id);
      return;
   }
   write_sse_headers(fd, request_id);
   char *data = (char *)malloc(PRESENCE_EVENT_DATA_MAX + 1);
   if (!data)
      return; /* headers already sent; just drop the stream */
   char ev[PRESENCE_EVENT_NAME_MAX];
   uint64_t cursor = 0;
   for (;;)
   {
      presence_wait_t w =
          presence_wait(id, &cursor, 1000, ev, sizeof(ev), data, PRESENCE_EVENT_DATA_MAX + 1);
      if (w == PRESENCE_WAIT_EVENT)
      {
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
      if (w == PRESENCE_WAIT_GONE)
         break;
      /* PRESENCE_WAIT_TIMEOUT: heartbeat; a failed write means the client
       * disconnected, so stop streaming and free the listener. */
      if (write_all_fd(fd, ": keep-alive\n\n", 13) < 0)
         break;
   }
   free(data);
}

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

static void handle_conn(int fd, int is_tcp)
{
   char buf[SHTTP_READ_MAX];
   int total = 0;
   while (total < SHTTP_READ_MAX - 1)
   {
      int n = (int)read(fd, buf + total, (size_t)(SHTTP_READ_MAX - 1 - total));
      if (n <= 0)
         break;
      total += n;
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
         break;
   }
   buf[total] = '\0';

   char method[16] = {0};
   char path[512] = {0};
   if (sscanf(buf, "%15s %511s", method, path) < 2)
   {
      send_response(fd, 400, "{\"error\":\"bad request\"}", NULL);
      return;
   }

   /* Strip any query string (unused by persona routes). */
   char *qmark = strchr(path, '?');
   if (qmark)
      *qmark = '\0';

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

   /* Establish the per-request context (#3) for this worker thread before any
    * handler runs. Overwritten on every request, so thread reuse cannot leak a
    * prior request's identity. */
   server_http_populate_request_context(fd, is_tcp, buf, request_id, method, path,
                                        server_http_conn_caps(is_tcp, g_bearer, g_remote_writes));

   /* Authorize before reading the body: TCP requires a valid bearer; the UDS
    * relies on filesystem permissions. A session-scoping key without a
    * configured bearer is refused on any transport. */
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
      int az = server_http_authorize(is_tcp, g_bearer, has_auth ? auth : NULL,
                                     has_api_key ? api_key : NULL, has_skey);
      if (az != 0)
      {
         const char *msg = az == 401 ? "{\"error\":{\"message\":\"missing or invalid bearer "
                                       "token\",\"type\":\"authentication_error\"}}"
                                     : "{\"error\":{\"message\":\"this endpoint requires a "
                                       "configured bearer token\",\"type\":\"server_error\"}}";
         send_response(fd, az, msg, request_id);
         LOG_INFO("server.http", "%s %s -> %d req_id=%s", method, path, az, request_id);
         return;
      }
   }

   /* Per-bearer rate limit on the TCP listener (the UDS listener is local and
    * never throttled). Connections are handled concurrently, so g_rate_state is
    * mutated under g_rate_lock. */
   if (is_tcp)
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
   if (is_tcp && !server_http_route_allowed(is_tcp, g_bearer, method, path, g_remote_writes))
   {
      send_response(fd, 403,
                    "{\"error\":{\"message\":\"this endpoint requires capabilities beyond the "
                    "presented token's scope\",\"type\":\"permission_error\"}}",
                    request_id);
      LOG_INFO("server.http", "%s %s -> 403 (caps) req_id=%s", method, path, request_id);
      return;
   }

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
            /* Offload to a detached worker so the long-lived presence SSE stream
             * does not block the listener thread (a subscriber would otherwise
             * freeze the whole /v1 surface); 503 if the live-stream cap is hit. */
            if (sse_offload(fd, handle_session_events, id, request_id) != 0)
               send_response(fd, 503, "{\"error\":\"too many event streams\"}", request_id);
            LOG_INFO("server.http", "GET %s -> presence events req_id=%s", path, request_id);
            return;
         }
      }
   }

   /* Body via Content-Length (read remainder after the header block). Header
    * names are case-insensitive (RFC 7230 §3.2): clients such as the Codex CLI
    * (reqwest/hyper) send a lowercase `content-length`, so match it via the
    * case-insensitive header lookup rather than a literal strstr — otherwise the
    * body is never read and large POSTs misparse as empty. */
   char *body = NULL;
   int body_len = 0;
   char clbuf[32] = "";
   if (http_header(buf, "Content-Length", clbuf, sizeof(clbuf)))
   {
      body_len = atoi(clbuf);
      if (body_len < 0)
         body_len = 0;
      if (body_len > SHTTP_MAX_BODY)
         body_len = SHTTP_MAX_BODY;
      body = malloc((size_t)body_len + 1);
      if (body)
      {
         int already = 0;
         const char *bs = strstr(buf, "\r\n\r\n");
         if (bs)
         {
            bs += 4;
            already = (int)(buf + total - bs);
            if (already < 0)
               already = 0;
            if (already > body_len)
               already = body_len;
            if (already > 0)
               memcpy(body, bs, (size_t)already);
         }
         while (already < body_len)
         {
            int n = (int)read(fd, body + already, (size_t)(body_len - already));
            if (n <= 0)
               break;
            already += n;
         }
         body[already] = '\0';
         body_len = already;
      }
   }

   /* Native streaming chat over HTTP — hands off to the async chat worker, which
    * streams newline-delimited aimee events. The outer route-allowed gate (TCP)
    * already enforces CAP_CHAT via server_http_route_caps; re-check here so the
    * UDS path is explicit too. */
   if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/chat/stream") == 0)
   {
      uint32_t need = server_capability_for_method("chat.send_stream");
      uint32_t have = server_http_conn_caps(is_tcp, g_bearer, g_remote_writes);
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
      config_t evcfg;
      config_load(&evcfg);
      if (evcfg.kb_evidence_emit_enabled)
      {
         char tid[40];
         ingress_preinject_mint_turn_id(tid, sizeof(tid));
         ingress_preinject_set_turn_id(tid);
      }
   }

   /* Streaming completions take the SSE path (separate from the buffered unary
    * route). With no stream handler registered the request falls through to the
    * unary handler, which rejects streaming. */
   if (strcmp(method, "POST") == 0 && openai_request_bool(body, "stream"))
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
   g_rpc_conn_caps = server_http_conn_caps(is_tcp, g_bearer, g_remote_writes);
   /* WP-C.0 hop 1 of 3: capture the attested vault identity (kernel UDS peer uid,
    * or a server.token-gated webuser assertion) into thread-locals, live until
    * loopback_rpc copies it into the synthesized conn. Cleared after the route so
    * a reused worker thread cannot leak it into the next request. */
   server_http_identity_capture(fd, is_tcp, buf);
   int status = server_http_route(method, path, body, body_len, resp, SHTTP_RESP_MAX);
   g_rpc_conn_caps = CAPS_READ_ONLY;
   server_http_identity_clear();
   send_response(fd, status, resp, request_id);
   LOG_INFO("server.http", "%s %s -> %d req_id=%s", method, path, status, request_id);
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
static atomic_int g_conn_live = 0;
#define CONN_LIVE_MAX 64

typedef struct
{
   int fd;
   int is_tcp;
} conn_job_t;

static void *conn_worker(void *arg)
{
   conn_job_t *j = (conn_job_t *)arg;
   handle_conn(j->fd, j->is_tcp);
   close(j->fd);
   atomic_fetch_sub(&g_conn_live, 1);
   free(j);
   return NULL;
}

/* Hand an accepted connection to a detached worker (which closes fd). Returns 1
 * if offloaded, 0 if the caller should handle it inline (cap hit / no
 * resources). */
static int conn_offload(int fd, int is_tcp)
{
   if (atomic_fetch_add(&g_conn_live, 1) >= CONN_LIVE_MAX)
   {
      atomic_fetch_sub(&g_conn_live, 1);
      return 0;
   }
   conn_job_t *j = (conn_job_t *)malloc(sizeof(*j));
   if (!j)
   {
      atomic_fetch_sub(&g_conn_live, 1);
      return 0;
   }
   j->fd = fd;
   j->is_tcp = is_tcp;
   pthread_attr_t attr;
   pthread_attr_t *ap = NULL;
   if (pthread_attr_init(&attr) == 0)
   {
      if (pthread_attr_setstacksize(&attr, (size_t)32 * 1024 * 1024) == 0)
         ap = &attr;
   }
   pthread_t t;
   int rc = pthread_create(&t, ap, conn_worker, j);
   if (ap)
      pthread_attr_destroy(&attr);
   if (rc != 0)
   {
      free(j);
      atomic_fetch_sub(&g_conn_live, 1);
      return 0;
   }
   pthread_detach(t);
   return 1;
}

/* Single accept loop over both listeners: poll the UDS (and the TCP fd when
 * bound), accept whichever is ready, and hand each connection to a per-
 * connection worker thread (conn_offload) so the accept loop never blocks. */
static void *listener_thread(void *arg)
{
   (void)arg;
   while (g_running)
   {
      struct pollfd pfds[2];
      int n = 0;
      int uds_idx = -1, tcp_idx = -1;
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
         int fd = accept(g_listen_fd, NULL, NULL);
         if (fd >= 0 && !conn_offload(fd, 0))
         {
            handle_conn(fd, 0);
            close(fd);
         }
      }
      if (tcp_idx >= 0 && (pfds[tcp_idx].revents & POLLIN))
      {
         int fd = accept(g_tcp_fd, NULL, NULL);
         if (fd >= 0 && !conn_offload(fd, 1))
         {
            handle_conn(fd, 1);
            close(fd);
         }
      }
   }
   return NULL;
}

/* Bind the optional localhost TCP listener. Returns the fd, or -1 (logged) when
 * disabled or on error — the UDS listener carries on regardless. */
static int tcp_listen(int tcp_port, const char *bearer_token)
{
   if (tcp_port <= 0)
      return -1;
   if (!bearer_token || !bearer_token[0])
   {
      LOG_WARN("server.http",
               "aimee.api.http_port=%d set but no bearer_token configured; "
               "refusing to bind TCP listener",
               tcp_port);
      return -1;
   }
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   int yes = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
   /* Loopback by default. Set AIMEE_SERVER_HTTP_BIND (any non-empty value) to
    * bind 0.0.0.0 instead — needed when the listener must be reachable from
    * outside the host (e.g. a container's published port, where traffic
    * arrives on a non-loopback interface). Mirrors aimee-kb's
    * AIMEE_KB_HTTP_BIND. The bearer requirement still applies on every request,
    * so a wider bind does not weaken auth. */
   const char *bind_all = getenv("AIMEE_SERVER_HTTP_BIND");
   in_addr_t bind_addr = (bind_all && bind_all[0]) ? INADDR_ANY : INADDR_LOOPBACK;
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

int server_http_start(const char *uds_path, int tcp_port, const char *bearer_token,
                      int rate_limit_per_min, int remote_writes)
{
   if (g_running || g_listen_fd >= 0)
      return -1;
   if (!uds_path || !uds_path[0])
      uds_path = server_http_default_path();

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", uds_path);

   unlink(uds_path);
   g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (g_listen_fd < 0)
      return -1;
   if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(g_listen_fd, SHTTP_BACKLOG) < 0)
   {
      close(g_listen_fd);
      g_listen_fd = -1;
      return -1;
   }

   /* Optional localhost TCP listener for OpenAI-style external tools. */
   g_bearer[0] = '\0';
   if (bearer_token && bearer_token[0])
      snprintf(g_bearer, sizeof(g_bearer), "%s", bearer_token);
   g_rate_limit = rate_limit_per_min > 0 ? rate_limit_per_min : 0;
   g_remote_writes = remote_writes;
   g_rate_state.window_start = 0;
   g_rate_state.count = 0;
   g_tcp_fd = tcp_listen(tcp_port, bearer_token);

   g_running = 1;
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
      g_running = 0;
      close(g_listen_fd);
      g_listen_fd = -1;
      if (g_tcp_fd >= 0)
      {
         close(g_tcp_fd);
         g_tcp_fd = -1;
      }
      unlink(uds_path);
      return -1;
   }
   pthread_detach(g_thread);
   if (g_tcp_fd >= 0)
   {
      const char *bind_all = getenv("AIMEE_SERVER_HTTP_BIND");
      LOG_INFO("server.http", "HTTP /v1 listening on %s and %s:%d (bearer)", uds_path,
               (bind_all && bind_all[0]) ? "0.0.0.0" : "127.0.0.1", tcp_port);
   }
   else
      LOG_INFO("server.http", "HTTP /v1 listening on %s", uds_path);
   return 0;
}

void server_http_stop(void)
{
   g_running = 0;
   if (g_listen_fd >= 0)
   {
      shutdown(g_listen_fd, SHUT_RDWR);
      close(g_listen_fd);
      g_listen_fd = -1;
   }
   if (g_tcp_fd >= 0)
   {
      shutdown(g_tcp_fd, SHUT_RDWR);
      close(g_tcp_fd);
      g_tcp_fd = -1;
   }
}
