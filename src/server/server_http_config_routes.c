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
#include "git_oauth_device.h"          /* GitLab/Gitea device-flow (relocated route handlers) */
#include <time.h>
#include "persona.h"
#include "roundtable_preset.h"
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
   /* Surface the per-role turn cap structurally so the Personas tab can render it
    * as a dedicated field (edited via the `max_turns:` frontmatter in `content`).
    * -1 = infinite, the default. */
   cJSON_AddNumberToObject(o, "max_turns", role_template_max_turns(name));
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

/* ── named roundtable presets (the roundtable analog of personas) ─────────────
 * Presets are stored one-per-file as JSON (roundtable_preset.{c,h}); the active
 * one is named by config.roundtable_default and its values are mirrored into the
 * live ensemble and roundtable config that the runtime reads. */

/* The current active-preset name (config.roundtable_default), or "" if none. */
static void rt_active_name(char *out, size_t out_n)
{
   config_t cfg;
   if (out && out_n)
      out[0] = '\0';
   if (config_load(&cfg) == 0 && out && out_n)
      snprintf(out, out_n, "%s", cfg.roundtable_default);
}

int route_roundtables_list(char *resp, int cap)
{
   enum
   {
      RT_LIST_MAX = 128
   };
   char names[RT_LIST_MAX][RT_PRESET_NAME_MAX];
   int n = roundtable_preset_list(names, RT_LIST_MAX);
   char active[RT_PRESET_NAME_MAX];
   rt_active_name(active, sizeof(active));

   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "roundtables");
   for (int i = 0; i < n; i++)
   {
      roundtable_preset_t p;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", names[i]);
      if (roundtable_preset_load(names[i], &p) == 0)
         cJSON_AddStringToObject(item, "description", p.description);
      cJSON_AddBoolToObject(item, "active", active[0] && strcmp(active, names[i]) == 0 ? 1 : 0);
      cJSON_AddItemToArray(arr, item);
   }
   /* Empty store: surface an implicit "default" synthesized from today's live
    * config so the GUI opens showing the effective roundtable rather than blank.
    * It is materialized only — saving it (PUT) is what writes a file. */
   if (n == 0)
   {
      const char *implicit = active[0] ? active : "default";
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", implicit);
      cJSON_AddStringToObject(item, "description", "current configuration (unsaved)");
      cJSON_AddBoolToObject(item, "active", 1);
      cJSON_AddBoolToObject(item, "synthesized", 1);
      cJSON_AddItemToArray(arr, item);
   }
   cJSON_AddStringToObject(root, "active", active);
   return emit(resp, cap, root);
}

int route_roundtable_show(const char *name, char *resp, int cap)
{
   if (!name || !name[0])
      return err_json(resp, cap, 400, "missing preset name");
   if (!roundtable_preset_name_valid(name))
      return err_json(resp, cap, 400, "invalid preset name");
   roundtable_preset_t p;
   if (roundtable_preset_load(name, &p) != 0)
   {
      /* Fall back to a synthesized preset from the live config when the store is
       * empty, so the implicit "default" shown by the list is fetchable. */
      char names[1][RT_PRESET_NAME_MAX];
      if (roundtable_preset_list(names, 1) == 0)
      {
         roundtable_preset_from_current_config(name, &p);
         cJSON *j = roundtable_preset_to_json(&p);
         cJSON_AddBoolToObject(j, "synthesized", 1);
         return emit(resp, cap, j);
      }
      return err_json(resp, cap, 404, "no such roundtable preset");
   }
   return emit(resp, cap, roundtable_preset_to_json(&p));
}

/* Create (POST, name in body) or edit (PUT /v1/roundtables/<name>) a preset. If
 * the saved preset is the active one, re-apply it so live config stays in sync. */
int route_roundtable_upsert(const char *url_name, const char *body, char *resp, int cap)
{
   roundtable_preset_t p;
   const char *errmsg = NULL;
   if (roundtable_preset_from_json(body, url_name, &p, &errmsg) != 0)
      return err_json(resp, cap, 400, errmsg ? errmsg : "bad request");
   if (roundtable_preset_save(&p) != 0)
      return err_json(resp, cap, 500, "failed to write roundtable preset");
   char active[RT_PRESET_NAME_MAX];
   rt_active_name(active, sizeof(active));
   if (active[0] && strcmp(active, p.name) == 0)
      (void)roundtable_preset_apply_to_config(p.name, NULL, 0);
   roundtable_preset_t loaded;
   if (roundtable_preset_load(p.name, &loaded) != 0)
      return err_json(resp, cap, 500, "failed to reload roundtable preset");
   return emit(resp, cap, roundtable_preset_to_json(&loaded));
}

int route_roundtable_remove(const char *name, char *resp, int cap)
{
   if (!name || !roundtable_preset_name_valid(name))
      return err_json(resp, cap, 400, "invalid preset name");
   if (roundtable_preset_delete(name) != 0)
      return err_json(resp, cap, 404, "no roundtable preset to remove");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "name", name);
   cJSON_AddBoolToObject(o, "deleted", 1);
   return emit(resp, cap, o);
}

/* POST /v1/roundtables/active {name}: make a preset the active roundtable — copy
 * its values into the live ensemble/roundtable config and persist. */
int route_roundtable_set_active(const char *body, char *resp, int cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *jn = req ? cJSON_GetObjectItemCaseSensitive(req, "name") : NULL;
   char name[RT_PRESET_NAME_MAX] = {0};
   if (cJSON_IsString(jn) && jn->valuestring)
      snprintf(name, sizeof(name), "%s", jn->valuestring);
   cJSON_Delete(req);
   if (!name[0] || !roundtable_preset_name_valid(name))
      return err_json(resp, cap, 400, "missing or invalid preset name");
   char err[128];
   if (roundtable_preset_apply_to_config(name, err, sizeof(err)) != 0)
      return err_json(resp, cap, 404, err[0] ? err : "could not activate preset");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "name", name);
   cJSON_AddBoolToObject(o, "active", 1);
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
void rh_query_str(const char *key, char *out, size_t cap)
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

/* ── GitLab/Gitea OAuth device-flow route handlers ──────────────────────────────
 * Relocated here from server_http_routes.c (referenced by that TU's route table
 * via server_http_internal.h) to keep it under the line-check ceiling. GitHub keeps
 * its dedicated handlers in server_http_routes.c. */

/* The git surface can be disabled at spawn (AIMEE_WEBCHAT_GIT=0); mirrors the
 * static gate in server_http_routes.c so these relocated handlers refuse the same. */
static int device_git_surface_enabled(void)
{
   const char *v = getenv("AIMEE_WEBCHAT_GIT");
   return !(v && v[0] == '0' && v[1] == '\0');
}

/* Resolve the device-flow provider (gitlab/gitea) + optional host from a JSON body.
 * Returns 0 on success, -1 (fills err) on an unknown/missing provider. */
static int device_provider_from_body(const cJSON *body, oauth_dev_provider_t *p, const char **host,
                                     char *err, size_t errlen)
{
   const cJSON *jp = body ? cJSON_GetObjectItemCaseSensitive(body, "provider") : NULL;
   const cJSON *jh = body ? cJSON_GetObjectItemCaseSensitive(body, "host") : NULL;
   const char *pn = (cJSON_IsString(jp) && jp->valuestring) ? jp->valuestring : NULL;
   *host = (cJSON_IsString(jh) && jh->valuestring) ? jh->valuestring : NULL;
   if (!pn || oauth_dev_provider_from_name(pn, p) != 0)
   {
      snprintf(err, errlen, "provider must be gitlab or gitea");
      return -1;
   }
   return 0;
}

int rh_git_oauth_device_start(const route_req_t *rq, char *resp, int cap)
{
   if (!device_git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git sign-in requires a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   oauth_dev_provider_t p;
   const char *host = NULL;
   char err[256];
   if (device_provider_from_body(body, &p, &host, err, sizeof(err)) != 0)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, err);
   }
   char user_code[64], verify_uri[256];
   int interval = 5;
   int rc = oauth_dev_start(p, host, principal, user_code, sizeof(user_code), verify_uri,
                            sizeof(verify_uri), &interval, err, sizeof(err));
   cJSON_Delete(body);
   if (rc != 0)
      return err_json(resp, cap, 502, err[0] ? err : "sign-in failed to start");

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", 1);
   cJSON_AddStringToObject(out, "user_code", user_code);
   cJSON_AddStringToObject(out, "verification_uri", verify_uri);
   cJSON_AddNumberToObject(out, "interval", interval);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

int rh_git_oauth_device_poll(const route_req_t *rq, char *resp, int cap)
{
   if (!device_git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git sign-in requires a webchat user");

   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
   oauth_dev_provider_t p;
   const char *host = NULL;
   char err[256] = "";
   if (device_provider_from_body(body, &p, &host, err, sizeof(err)) != 0)
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, err);
   }
   int rc = oauth_dev_poll(p, host, principal, err, sizeof(err));
   cJSON_Delete(body);
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "status", rc == 1 ? "done" : rc == 0 ? "pending" : "error");
   if (rc < 0 && err[0])
      cJSON_AddStringToObject(out, "error", err);
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}

int rh_git_oauth_device_config(const route_req_t *rq, char *resp, int cap)
{
   if (!device_git_surface_enabled())
      return err_json(resp, cap, 503, "the git surface is disabled on this server");
   const char *principal = server_http_identity_principal();
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return err_json(resp, cap, 403, "git sign-in requires a webchat user");

   if (strcmp(rq->method, "POST") == 0)
   {
      cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : NULL;
      oauth_dev_provider_t p;
      const char *host = NULL;
      char err[256];
      if (device_provider_from_body(body, &p, &host, err, sizeof(err)) != 0)
      {
         cJSON_Delete(body);
         return err_json(resp, cap, 400, err);
      }
      const cJSON *jid = cJSON_GetObjectItemCaseSensitive(body, "client_id");
      const char *id = (cJSON_IsString(jid) && jid->valuestring) ? jid->valuestring : NULL;
      int rc = (id && id[0]) ? oauth_dev_set_client_id(p, host, id) : -1;
      cJSON_Delete(body);
      if (rc != 0)
         return err_json(resp, cap, 400, "client_id required");
      return snprintf(resp, (size_t)cap, "{\"ok\":true}") < cap
                 ? 200
                 : err_json(resp, cap, 500, "too large");
   }

   char provider[32], host[256];
   rh_query_str("provider", provider, sizeof(provider));
   rh_query_str("host", host, sizeof(host));
   oauth_dev_provider_t p;
   if (oauth_dev_provider_from_name(provider, &p) != 0)
      return err_json(resp, cap, 400, "provider must be gitlab or gitea");
   char id[256];
   int have = oauth_dev_get_client_id(p, host[0] ? host : NULL, id, sizeof(id));
   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "configured", have);
   cJSON_AddStringToObject(out, "client_id", have ? id : "");
   char *s = cJSON_PrintUnformatted(out);
   int n = s ? snprintf(resp, (size_t)cap, "%s", s) : -1;
   free(s);
   cJSON_Delete(out);
   return (n > 0 && n < cap) ? 200 : err_json(resp, cap, 500, "response too large");
}
