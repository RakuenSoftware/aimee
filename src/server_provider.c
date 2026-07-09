/* server_provider.c: split from server.c into a real translation unit
 * (was server_provider.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
#include "harness_memory_audit.h"  /* hmem_audit */
#include "harness_memory_common.h" /* hmem_resolve_project / hmem_project_key_ok */
#include "harness_memory_scope.h"  /* hmem_scope_for_client */
#include "json_fluent.h"           /* jo_ok */
#include "memory_redirect.h"       /* memory_redirect_classify / _bash_targets / _rematerialize */
#include "server.h"
#include "turn_registry.h"
#include "server_http.h" /* server_http_api_status_report */
#include "config.h"      /* config_t / config_load for api.status, api.enable */
#include "delegate_backend_docker.h"
#include "delegate_backend_local.h"
#include "delegate_backend_ssh.h"
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include "skill_review.h"
#include "trigger_scheduler.h"
#include "wfe_live_delegate.h"
#include "wfe_scheduler.h"
#include "server_trigger.h"
#include "server_cron.h"
#include "server_pipeline.h" /* roundtable authoring pipeline (pipeline.*) */
#include "commands.h"
#include "agent.h"
#include "agent_exec.h"     /* agent_audit_async_flush — drain audit queue at shutdown */
#include "webuser_editor.h" /* webuser_editor_shutdown — reap editors at shutdown (WP-I) */
#include "agent_config.h"
#include "provider_catalog.h"
#include "delegate_credentials.h"
#include "model_registry.h"
#include "model_provider.h"
#include "model_registry.h"
#include "db1.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include "workspace.h"
#include "worktree_gc.h"
#include "git_verify.h"
#include "toolset.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

static const char *server_provider_first_env_var(const model_provider_t *p)
{
   if (!p || !p->env_vars)
      return NULL;
   for (int i = 0; p->env_vars[i]; i++)
      if (p->env_vars[i] && p->env_vars[i][0])
         return p->env_vars[i];
   return NULL;
}

static int server_provider_has_credentials(const model_provider_t *p)
{
   if (!p)
      return 0;
   if (p->auth_type && strcmp(p->auth_type, "none") == 0)
      return 1;
   if (!p->env_vars)
      return 0;
   for (int i = 0; p->env_vars[i]; i++)
   {
      const char *val = getenv(p->env_vars[i]);
      if (val && val[0])
         return 1;
   }
   return 0;
}

static void server_provider_free_models(char **models, int n)
{
   if (!models)
      return;
   for (int i = 0; i < n; i++)
      free(models[i]);
   free(models);
}

#define PROVIDER_MODEL_CATALOG_TTL_SECONDS 3600

static int server_provider_models_cached(model_provider_t *p, char ***models_out, int *n_out)
{
   *models_out = NULL;
   *n_out = 0;
   if (db1_model_catalog_is_fresh(p->name, PROVIDER_MODEL_CATALOG_TTL_SECONDS) &&
       db1_model_catalog_get(p->name, models_out, n_out) == 0)
      return 0;

   if (p->fetch_models && p->fetch_models(p, models_out, n_out) == 0)
   {
      (void)db1_model_catalog_replace(p->name, *models_out, *n_out);
      return 0;
   }

   return db1_model_catalog_get(p->name, models_out, n_out);
}

static cJSON *server_provider_json(const model_provider_t *p)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddStringToObject(obj, "name", p->name ? p->name : "");
   cJSON_AddStringToObject(obj, "display_name", p->display_name ? p->display_name : "");
   cJSON_AddStringToObject(obj, "description", p->description ? p->description : "");
   cJSON_AddStringToObject(obj, "base_url", p->base_url ? p->base_url : "");
   cJSON_AddStringToObject(obj, "models_url", p->models_url ? p->models_url : "");
   cJSON_AddStringToObject(obj, "signup_url", p->signup_url ? p->signup_url : "");
   cJSON_AddStringToObject(obj, "auth_type", p->auth_type ? p->auth_type : "");
   cJSON_AddStringToObject(
       obj, "env_var", server_provider_first_env_var(p) ? server_provider_first_env_var(p) : "");
   cJSON_AddStringToObject(obj, "default_model", p->default_model ? p->default_model : "");
   cJSON_AddStringToObject(obj, "default_aux_model",
                           p->default_aux_model ? p->default_aux_model : "");
   cJSON_AddBoolToObject(obj, "available", server_provider_has_credentials(p));

   cJSON *envs = cJSON_CreateArray();
   if (envs)
   {
      if (p->env_vars)
      {
         for (int i = 0; p->env_vars[i]; i++)
            cJSON_AddItemToArray(envs, cJSON_CreateString(p->env_vars[i]));
      }
      cJSON_AddItemToObject(obj, "env_vars", envs);
   }
   return obj;
}

int handle_provider_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int available_only = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "available_only"));
   int json_output = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "json"));
   model_provider_t *providers[64];
   int n = model_provider_list(providers, 64);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "json", json_output);
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      if (available_only && !server_provider_has_credentials(providers[i]))
         continue;
      cJSON_AddItemToArray(arr, server_provider_json(providers[i]));
   }
   cJSON_AddItemToObject(resp, "providers", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_provider_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return server_send_error(conn, "provider name required", NULL);
   model_provider_t *p = model_provider_get(jname->valuestring);
   if (!p)
      return server_send_error(conn, "provider not found", NULL);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "provider", server_provider_json(p));
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_provider_models(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return server_send_error(conn, "provider name required", NULL);
   model_provider_t *p = model_provider_get(jname->valuestring);
   if (!p)
      return server_send_error(conn, "provider not found", NULL);
   if (!p->fetch_models)
      return server_send_error(conn, "provider does not support model listing", NULL);
   char **models = NULL;
   int n = 0;
   if (server_provider_models_cached(p, &models, &n) != 0)
      return server_send_error(conn, "failed to fetch models", NULL);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "provider", p->name);
   cJSON_AddBoolToObject(resp, "json", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "json")));
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(models[i]));
   cJSON_AddItemToObject(resp, "models", arr);
   cJSON_AddNumberToObject(resp, "count", n);
   server_provider_free_models(models, n);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_provider_test(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return server_send_error(conn, "provider name required", NULL);
   model_provider_t *p = model_provider_get(jname->valuestring);
   if (!p)
      return server_send_error(conn, "provider not found", NULL);
   if (!server_provider_has_credentials(p))
      return server_send_error(conn, "provider credentials unavailable", NULL);

   char message[160];
   if (!p->fetch_models)
      snprintf(message, sizeof(message), "%s: credentials available; no probe registered", p->name);
   else
   {
      char **models = NULL;
      int n = 0;
      if (p->fetch_models(p, &models, &n) != 0)
         return server_send_error(conn, "provider probe failed", NULL);
      server_provider_free_models(models, n);
      snprintf(message, sizeof(message), "%s: ok (%d models)", p->name, n);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "provider", p->name);
   cJSON_AddStringToObject(resp, "message", message);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_provider_quota(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
   const char *name = cJSON_IsString(jname) && jname->valuestring[0] ? jname->valuestring : NULL;
   char quota[4096];
   int n = delegate_credentials_format_quota(name, quota, sizeof(quota));
   if (n < 0)
      return server_send_error(conn, "failed to format credential quota state", NULL);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   if (name)
      cJSON_AddStringToObject(resp, "provider", name);
   cJSON_AddNumberToObject(resp, "credential_count", n);
   cJSON_AddStringToObject(resp, "quota", quota);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_provider_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   config_t cfg;
   config_load(&cfg);
   const char *name = cfg.provider[0] ? cfg.provider : "claude";
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "provider", name);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_provider_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return server_send_error(conn, "provider name required", NULL);
   const char *name = jname->valuestring;
   if (strlen(name) >= 16)
      return server_send_error(conn, "provider name too long (max 15 chars)", NULL);
   config_t cfg;
   config_load(&cfg);
   snprintf(cfg.provider, sizeof(cfg.provider), "%s", name);
   if (config_save(&cfg) != 0)
      return server_send_error(conn, "failed to save config", NULL);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "provider", cfg.provider);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

static cJSON *server_model_capability_json(const model_capability_t *cap)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   char flags[128];
   model_capability_flags_string(cap->flags, flags, sizeof(flags));
   cJSON_AddStringToObject(obj, "provider", cap->provider);
   cJSON_AddStringToObject(obj, "model", cap->model_id);
   cJSON_AddNumberToObject(obj, "context_window", cap->context_window);
   cJSON_AddNumberToObject(obj, "max_output", cap->max_output);
   cJSON_AddNumberToObject(obj, "cost_in_per_mtok", cap->cost_in_per_mtok);
   cJSON_AddNumberToObject(obj, "cost_out_per_mtok", cap->cost_out_per_mtok);
   cJSON_AddNumberToObject(obj, "flags_mask", cap->flags);
   cJSON_AddStringToObject(obj, "flags", flags);
   cJSON_AddStringToObject(obj, "capabilities", flags);
   cJSON_AddStringToObject(obj, "modalities", cap->modalities);
   cJSON_AddStringToObject(obj, "knowledge_cutoff", cap->knowledge_cutoff);
   cJSON_AddBoolToObject(obj, "open_weights", cap->open_weights);
   cJSON_AddBoolToObject(obj, "deprecated", cap->deprecated);
   return obj;
}

int handle_model_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *cap_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "capability"));
   unsigned required = 0;
   if (cap_name && cap_name[0])
   {
      required = model_capability_flag_from_name(cap_name);
      if (!required)
         return server_send_error(conn, "unknown model capability", NULL);
   }
   int open_weights_only = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "open_weights_only"));

   model_capability_t caps[256];
   int total = model_capability_list(caps, 256, required, open_weights_only);
   int n = total < 256 ? total : 256;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", total);
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, server_model_capability_json(&caps[i]));
   cJSON_AddItemToObject(resp, "models", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_model_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "name"));
   const char *provider = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "provider"));
   const char *model = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "model"));
   model_capability_t cap;
   char resolved_provider[MODEL_PROVIDER_MAX];
   char resolved_model[MODEL_ID_MAX];
   if (name && name[0])
   {
      if (!model_capability_resolve_ref(name, resolved_provider, sizeof(resolved_provider),
                                        resolved_model, sizeof(resolved_model), &cap))
         return server_send_error(conn, "model capability lookup failed", NULL);
   }
   else
   {
      if (!model || !model[0])
         return server_send_error(conn, "model required", NULL);
      if (!model_capability_get(provider, model, &cap))
         return server_send_error(conn, "model capability metadata not found", NULL);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "model", server_model_capability_json(&cap));
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_model_refresh(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char msg[256] = "";
   int n = model_capability_refresh(msg, sizeof(msg));
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "message", msg[0] ? msg : "model metadata refreshed");
   cJSON_AddNumberToObject(resp, "count", n > 0 ? n : 0);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
