/* server_http_config_routes.c: split from server_http.c into a real translation unit
 * (was server_http_config_routes.inc, textually included only to stay under the
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
#include "wfe_scheduler.h"        /* wfe_scheduler_notify — resume the autonomy driver */
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

cJSON *persona_to_json(const persona_t *p)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "name", p->name);
   cJSON_AddStringToObject(o, "description", p->description);
   cJSON *roles = cJSON_AddArrayToObject(o, "roles");
   for (int i = 0; i < p->roles_count; i++)
      cJSON_AddItemToArray(roles, cJSON_CreateString(p->roles[i]));
   cJSON_AddStringToObject(o, "check_role", p->check_role);
   cJSON_AddStringToObject(o, "check_marker", p->check_marker);
   cJSON_AddStringToObject(o, "delegates", p->delegates);
   cJSON_AddBoolToObject(o, "builtin", p->builtin);
   /* Full prose so clients can show/round-trip a persona for editing. Built-ins
    * loaded via the code fallback (no file) carry NULL prose; emit "" there. */
   cJSON_AddStringToObject(o, "persona", p->persona_text ? p->persona_text : "");
   cJSON_AddStringToObject(o, "principles", p->principles_text ? p->principles_text : "");
   cJSON_AddStringToObject(o, "brief", p->brief_text ? p->brief_text : "");
   return o;
}

/* Parse a persona create/edit request body into *out (zeroed first). Heap prose
 * fields are owned by the caller (persona_free). `url_name`, when non-empty
 * (from PUT /v1/personas/<name>), takes precedence over a body "name". Returns 0
 * on success; -1 with *errmsg set on a validation error. */
static int persona_from_json(const char *body, const char *url_name, persona_t *out,
                             const char **errmsg)
{
   memset(out, 0, sizeof(*out));
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   if (!req)
   {
      *errmsg = "invalid JSON body";
      return -1;
   }
   const char *name = (url_name && url_name[0]) ? url_name : NULL;
   if (!name)
   {
      cJSON *jn = cJSON_GetObjectItemCaseSensitive(req, "name");
      name = cJSON_IsString(jn) ? jn->valuestring : NULL;
   }
   if (!name || !persona_name_valid(name))
   {
      cJSON_Delete(req);
      *errmsg = "missing or invalid persona name";
      return -1;
   }
   snprintf(out->name, sizeof(out->name), "%s", name);

   cJSON *d = cJSON_GetObjectItemCaseSensitive(req, "description");
   if (cJSON_IsString(d))
      snprintf(out->description, sizeof(out->description), "%s", d->valuestring);
   cJSON *dg = cJSON_GetObjectItemCaseSensitive(req, "delegates");
   if (cJSON_IsString(dg) && dg->valuestring[0])
      snprintf(out->delegates, sizeof(out->delegates), "%s", dg->valuestring);
   else
      snprintf(out->delegates, sizeof(out->delegates), "%s", PERSONA_DELEGATES_FULL);
   cJSON *cr = cJSON_GetObjectItemCaseSensitive(req, "check_role");
   if (cJSON_IsString(cr))
      snprintf(out->check_role, sizeof(out->check_role), "%s", cr->valuestring);
   cJSON *cm = cJSON_GetObjectItemCaseSensitive(req, "check_marker");
   if (cJSON_IsString(cm))
      snprintf(out->check_marker, sizeof(out->check_marker), "%s", cm->valuestring);
   cJSON *roles = cJSON_GetObjectItemCaseSensitive(req, "roles");
   if (cJSON_IsArray(roles))
   {
      cJSON *r = NULL;
      cJSON_ArrayForEach(r, roles)
      {
         if (cJSON_IsString(r) && out->roles_count < PERSONA_MAX_ROLES)
            snprintf(out->roles[out->roles_count++], PERSONA_NAME_MAX, "%s", r->valuestring);
      }
   }
   cJSON *pt = cJSON_GetObjectItemCaseSensitive(req, "persona");
   if (cJSON_IsString(pt) && pt->valuestring[0])
      out->persona_text = safe_strdup(pt->valuestring);
   cJSON *pr = cJSON_GetObjectItemCaseSensitive(req, "principles");
   if (cJSON_IsString(pr) && pr->valuestring[0])
      out->principles_text = safe_strdup(pr->valuestring);
   cJSON *br = cJSON_GetObjectItemCaseSensitive(req, "brief");
   if (cJSON_IsString(br) && br->valuestring[0])
      out->brief_text = safe_strdup(br->valuestring);
   cJSON_Delete(req);
   return 0;
}

/* ── routes ─────────────────────────────────────────────────────────────── */

int route_personas_list(char *resp, int cap)
{
   char names[PERSONA_MAX_NAMES][PERSONA_NAME_MAX];
   int n = persona_list(NULL, names, PERSONA_MAX_NAMES);
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "personas");
   for (int i = 0; i < n; i++)
   {
      persona_t p;
      if (persona_load(NULL, names[i], &p) != 0)
         continue;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", p.name);
      cJSON_AddStringToObject(item, "description", p.description);
      cJSON_AddBoolToObject(item, "builtin", p.builtin);
      cJSON_AddItemToArray(arr, item);
      persona_free(&p);
   }
   return emit(resp, cap, root);
}

/* The active durable-default persona (resolved server-side from config/env), so
 * non-session clients (e.g. `aimee manuscript check`) never read config files. */
int route_persona_current(char *resp, int cap)
{
   const char *name = aimee_mode_to_string(config_current_mode());
   persona_t p;
   persona_load(NULL, name, &p);
   int rc = emit(resp, cap, persona_to_json(&p));
   persona_free(&p);
   return rc;
}

int route_persona_show(const char *name, char *resp, int cap)
{
   if (!name || !name[0])
      return err_json(resp, cap, 400, "missing persona name");
   if (!persona_is_builtin(name))
   {
      char path[PERSONA_PATH_MAX];
      if (persona_path(NULL, name, path, sizeof(path)) != 0)
         return err_json(resp, cap, 404, "no such persona");
   }
   persona_t p;
   if (persona_load(NULL, name, &p) != 0)
      return err_json(resp, cap, 500, "load failed");
   int rc = emit(resp, cap, persona_to_json(&p));
   persona_free(&p);
   return rc;
}

/* Create or edit a persona: write <config>/personas/<name>.md from the request
 * body, then return the reloaded persona (config is the source of truth). Used
 * by POST /v1/personas (name in body) and PUT /v1/personas/<name>. Editing a
 * built-in name writes an override file the built-in default falls back to when
 * removed. */
int route_persona_upsert(const char *url_name, const char *body, char *resp, int cap)
{
   persona_t p;
   const char *errmsg = NULL;
   if (persona_from_json(body, url_name, &p, &errmsg) != 0)
      return err_json(resp, cap, 400, errmsg ? errmsg : "bad request");
   int wrote = persona_write(&p);
   persona_free(&p);
   if (wrote != 0)
      return err_json(resp, cap, 500, "failed to write persona");
   persona_t loaded;
   persona_load(NULL, p.name, &loaded);
   int rc = emit(resp, cap, persona_to_json(&loaded));
   persona_free(&loaded);
   return rc;
}

/* Remove a user-level persona file (DELETE /v1/personas/<name>). For a built-in
 * this resets it to the code default; for a custom persona it deletes it. */
int route_persona_remove(const char *name, char *resp, int cap)
{
   if (!name || !persona_name_valid(name))
      return err_json(resp, cap, 400, "invalid persona name");
   if (persona_delete(name) != 0)
      return err_json(resp, cap, 404, "no user persona file to remove");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "name", name);
   cJSON_AddBoolToObject(o, "deleted", 1);
   return emit(resp, cap, o);
}

/* ── delegate role templates (the delegate analog of personas) ───────────── */

int route_role_templates_list(char *resp, int cap)
{
   char names[ROLE_TEMPLATE_MAX_ROLES][ROLE_TEMPLATE_NAME_MAX];
   int n = role_template_list(NULL, names, ROLE_TEMPLATE_MAX_ROLES);
   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "role_templates");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
   return emit(resp, cap, root);
}

int route_role_template_show(const char *name, char *resp, int cap)
{
   if (!name || !name[0])
      return err_json(resp, cap, 400, "missing role name");
   char *raw = role_template_read_raw(NULL, name);
   if (!raw)
      return err_json(resp, cap, 404, "no such role template");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "role", name);
   cJSON_AddStringToObject(o, "content", raw);
   free(raw);
   return emit(resp, cap, o);
}

int route_role_template_upsert(const char *name, const char *body, char *resp, int cap)
{
   if (!role_template_name_valid(name))
      return err_json(resp, cap, 400, "invalid role name");
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *jc = req ? cJSON_GetObjectItemCaseSensitive(req, "content") : NULL;
   if (!cJSON_IsString(jc))
   {
      cJSON_Delete(req);
      return err_json(resp, cap, 400, "missing 'content' string");
   }
   int rc = role_template_write(name, jc->valuestring);
   cJSON_Delete(req);
   if (rc != 0)
      return err_json(resp, cap, 500, "failed to write role template");
   char *raw = role_template_read_raw(NULL, name);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "role", name);
   cJSON_AddStringToObject(o, "content", raw ? raw : "");
   free(raw);
   return emit(resp, cap, o);
}

int route_role_template_remove(const char *name, char *resp, int cap)
{
   if (!role_template_name_valid(name))
      return err_json(resp, cap, 400, "invalid role name");
   if (role_template_delete(name) != 0)
      return err_json(resp, cap, 404, "no user role template to remove");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "role", name);
   cJSON_AddBoolToObject(o, "deleted", 1);
   return emit(resp, cap, o);
}

/* ── query-param helpers + Workflow Actions route adapters ───────────────────
 * Relocated here from server_http_routes.c (referenced by that TU's route table
 * via server_http_internal.h) to keep that file under the line-check ceiling. */

/* Parse an unsigned long query param ("k=v&…") from the request's query string;
 * returns `dflt` when the key is absent or unparseable. */
long rh_query_long(const char *key, long dflt)
{
   const char *q = server_http_identity_query();
   size_t klen = strlen(key);
   for (const char *p = q; p && *p;)
   {
      const char *amp = strchr(p, '&');
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         char *end = NULL;
         long v = strtol(p + klen + 1, &end, 10);
         if (end != p + klen + 1)
            return v;
         return dflt;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   return dflt;
}

/* One hex digit → value, or -1. */
static int rh_hex(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Read a percent-decoded string query param into `out` ("" if absent). A path may
 * arrive percent-encoded (encodeURIComponent turns '/' into %2F), so %XX escapes
 * are decoded; a malformed escape is copied literally. */
static void rh_query_str(const char *key, char *out, size_t cap)
{
   if (cap)
      out[0] = '\0';
   const char *q = server_http_identity_query();
   size_t klen = strlen(key);
   for (const char *p = q; p && *p;)
   {
      const char *amp = strchr(p, '&');
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         const char *v = p + klen + 1;
         const char *end = amp ? amp : v + strlen(v);
         size_t o = 0;
         for (const char *s = v; s < end && cap && o + 1 < cap; s++)
         {
            int hi, lo;
            if (*s == '%' && s + 2 < end && (hi = rh_hex(s[1])) >= 0 && (lo = rh_hex(s[2])) >= 0)
            {
               out[o++] = (char)((hi << 4) | lo);
               s += 2;
            }
            else
            {
               out[o++] = *s;
            }
         }
         if (cap)
            out[o] = '\0';
         return;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
}

/* Lifecycle mutations. The route cap (CAP_DASHBOARD_READ) admits owners; operator
 * status (CAP_WORKFLOW_ADMIN on the connection) lifts the owner-only restriction
 * inside the wf_api_* handler. */
#define RH_WF_IS_OPERATOR() ((g_rpc_conn_caps & CAP_WORKFLOW_ADMIN) != 0)
int rh_wf_item_pause(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_item_pause(rq->id, RH_WF_IS_OPERATOR(), resp, cap);
}
int rh_wf_item_resume(const route_req_t *rq, char *resp, int cap)
{
   int rc = wf_api_item_resume(rq->id, RH_WF_IS_OPERATOR(), resp, cap);
   if (rc >= 200 && rc < 300)
      wfe_scheduler_notify(); /* wake the driver so the resumed run advances now */
   return rc;
}
int rh_wf_item_stop(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_item_stop(rq->id, RH_WF_IS_OPERATOR(), resp, cap);
}
int rh_wf_item_delete(const route_req_t *rq, char *resp, int cap)
{
   return wf_api_item_delete(rq->id, RH_WF_IS_OPERATOR(), resp, cap);
}
int rh_wf_repo_tree(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   char path[4096];
   rh_query_str("path", path, sizeof path);
   return wf_api_repo_tree(path, resp, cap);
}
int rh_wf_repo_file(const route_req_t *rq, char *resp, int cap)
{
   (void)rq;
   char path[4096];
   rh_query_str("path", path, sizeof path);
   return wf_api_repo_file(path, resp, cap);
}
