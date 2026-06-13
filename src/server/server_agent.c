/* server_agent.c: agent management RPC handlers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "server.h"
#include "commands.h"
#include "agent.h"
#include "agent_adapter.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "log.h"
#include "vault_service.h" /* vault_service_set / set_server, VAULT_API_KEY_CRED */
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "platform_path.h"

/* --- Agent management RPCs --- */

static pthread_once_t g_agent_http_once = PTHREAD_ONCE_INIT;
#define SERVER_AGENT_MAX_ARGS 64

static void server_agent_http_init_once(void)
{
   agent_http_init();
}

static void server_agent_http_ensure(void)
{
   pthread_once(&g_agent_http_once, server_agent_http_init_once);
}

static char *server_agent_trim(char *s)
{
   if (!s)
      return s;
   while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
      s++;
   char *end = s + strlen(s);
   while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
      *--end = '\0';
   return s;
}

static void server_agent_set_roles_csv(agent_t *ag, const char *csv)
{
   ag->role_count = 0;
   if (!csv || !csv[0])
      return;
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *tok = strtok(buf, ",");
   while (tok && ag->role_count < MAX_AGENT_ROLES)
   {
      char *role = server_agent_trim(tok);
      if (role[0])
         snprintf(ag->roles[ag->role_count++], sizeof(ag->roles[0]), "%s", role);
      tok = strtok(NULL, ",");
   }
}

static void server_agent_set_exec_roles_csv(agent_t *ag, const char *csv)
{
   ag->exec_role_count = 0;
   if (!csv || !csv[0])
      return;
   char buf[256];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *tok = strtok(buf, ",");
   while (tok && ag->exec_role_count < MAX_EXEC_ROLES)
   {
      char *role = server_agent_trim(tok);
      if (role[0])
         snprintf(ag->exec_roles[ag->exec_role_count++], sizeof(ag->exec_roles[0]), "%s", role);
      tok = strtok(NULL, ",");
   }
}

static void server_agent_default_roles(agent_t *ag)
{
   server_agent_set_roles_csv(ag,
                              "code,review,explain,refactor,draft,execute,summarize,format,reason,"
                              "search");
}

static int server_agent_looks_endpoint(const char *s)
{
   return s && s[0] &&
          (strstr(s, "://") || strchr(s, ':') || strchr(s, '.') || strcmp(s, "localhost") == 0 ||
           strcmp(s, "127.0.0.1") == 0);
}

static void server_agent_normalize_endpoint(const char *input, char *out, size_t out_len)
{
   char tmp[MAX_ENDPOINT_LEN];
   if (strstr(input, "://"))
      snprintf(tmp, sizeof(tmp), "%s", input);
   else
      snprintf(tmp, sizeof(tmp), "http://%s", input);

   size_t len = strlen(tmp);
   while (len > 0 && tmp[len - 1] == '/')
      tmp[--len] = '\0';

   char *v1 = strstr(tmp, "/v1");
   if (v1 && (v1[3] == '\0' || v1[3] == '/'))
   {
      v1[3] = '\0';
      snprintf(out, out_len, "%s", tmp);
      return;
   }
   snprintf(out, out_len, "%s/v1", tmp);
}

static void server_agent_endpoint_root(const char *endpoint, char *out, size_t out_len)
{
   snprintf(out, out_len, "%s", endpoint ? endpoint : "");
   size_t len = strlen(out);
   if (len >= 3 && strcmp(out + len - 3, "/v1") == 0)
      out[len - 3] = '\0';
}

static void server_agent_join_url(const char *base, const char *suffix, char *out, size_t out_len)
{
   snprintf(out, out_len, "%s%s%s", base,
            (base && base[0] && base[strlen(base) - 1] == '/') ? "" : "/", suffix);
}

static int server_agent_json_int(cJSON *obj, const char *name)
{
   cJSON *v = cJSON_GetObjectItem(obj, name);
   return cJSON_IsNumber(v) ? v->valueint : 0;
}

static void server_agent_parse_slots(cJSON *root, int *slots_out, int *ctx_out)
{
   *slots_out = 0;
   *ctx_out = 0;
   if (cJSON_IsObject(root))
   {
      cJSON *arr = cJSON_GetObjectItem(root, "slots");
      if (cJSON_IsArray(arr))
         root = arr;
      else
      {
         int slots = server_agent_json_int(root, "slots");
         if (!slots)
            slots = server_agent_json_int(root, "n_slots");
         if (!slots)
            slots = server_agent_json_int(root, "parallel");
         int ctx = server_agent_json_int(root, "n_ctx");
         if (!ctx)
            ctx = server_agent_json_int(root, "n_ctx_slot");
         if (!ctx)
            ctx = server_agent_json_int(root, "context_window");
         *slots_out = slots;
         *ctx_out = ctx;
         return;
      }
   }
   if (!cJSON_IsArray(root))
      return;
   int n = cJSON_GetArraySize(root);
   int min_ctx = 0;
   for (int i = 0; i < n; i++)
   {
      cJSON *slot = cJSON_GetArrayItem(root, i);
      if (!cJSON_IsObject(slot))
         continue;
      int ctx = server_agent_json_int(slot, "n_ctx");
      if (!ctx)
         ctx = server_agent_json_int(slot, "n_ctx_slot");
      if (!ctx)
         ctx = server_agent_json_int(slot, "context_window");
      if (ctx > 0 && (!min_ctx || ctx < min_ctx))
         min_ctx = ctx;
   }
   *slots_out = n;
   *ctx_out = min_ctx;
}

static int server_agent_probe_models(const char *endpoint, const char *auth_header,
                                     const char *requested_model, char *model_out, size_t model_len,
                                     int *model_available, char *errbuf, size_t errbuf_len)
{
   model_out[0] = '\0';
   if (model_available)
      *model_available = 0;

   char url[MAX_ENDPOINT_LEN + 32];
   server_agent_join_url(endpoint, "models", url, sizeof(url));

   char *body = NULL;
   int status = agent_http_get(url, auth_header, &body, 5000);
   if (status != 200 || !body)
   {
      snprintf(errbuf, errbuf_len, "GET %s returned %d", url, status);
      free(body);
      return status;
   }

   cJSON *root = cJSON_Parse(body);
   free(body);
   if (!root)
   {
      snprintf(errbuf, errbuf_len, "GET %s returned invalid JSON", url);
      return status;
   }

   cJSON *data = cJSON_GetObjectItem(root, "data");
   if (!cJSON_IsArray(data))
      data = cJSON_GetObjectItem(root, "models");
   if (cJSON_IsArray(data))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, data)
      {
         const char *id = cJSON_IsString(item)
                              ? item->valuestring
                              : cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
         if (!id || !id[0])
            continue;
         if (!model_out[0])
         {
            snprintf(model_out, model_len, "%s", id);
            if ((!requested_model || !requested_model[0]) && model_available)
               *model_available = 1;
         }
         if (requested_model && requested_model[0] && strcmp(id, requested_model) == 0)
         {
            if (model_available)
               *model_available = 1;
            snprintf(model_out, model_len, "%s", id);
            break;
         }
      }
   }

   if (requested_model && requested_model[0] && model_available && !*model_available)
      snprintf(errbuf, errbuf_len, "model '%s' was not listed by %s", requested_model, url);
   else if (!model_out[0])
      snprintf(errbuf, errbuf_len, "no model ids found at %s", url);
   cJSON_Delete(root);
   return status;
}

static int server_agent_probe_slots(const char *endpoint, const char *auth_header, int *slots_out,
                                    int *ctx_out, char *errbuf, size_t errbuf_len)
{
   *slots_out = 0;
   *ctx_out = 0;
   char root[MAX_ENDPOINT_LEN];
   char url[MAX_ENDPOINT_LEN + 32];
   server_agent_endpoint_root(endpoint, root, sizeof(root));
   server_agent_join_url(root, "slots", url, sizeof(url));

   char *body = NULL;
   int status = agent_http_get(url, auth_header, &body, 5000);
   if (status != 200 || !body)
   {
      snprintf(errbuf, errbuf_len, "GET %s returned %d", url, status);
      free(body);
      return status;
   }

   cJSON *json = cJSON_Parse(body);
   free(body);
   if (!json)
   {
      snprintf(errbuf, errbuf_len, "GET %s returned invalid JSON", url);
      return status;
   }
   server_agent_parse_slots(json, slots_out, ctx_out);
   if (*slots_out <= 0)
      snprintf(errbuf, errbuf_len, "no slots found at %s", url);
   cJSON_Delete(json);
   return status;
}

static int server_agent_should_probe_slots(const agent_t *ag)
{
   if (!ag || !ag->endpoint[0])
      return 0;
   if (ag->backend[0])
      return 0;
   if (ag->auth_type[0] && strcmp(ag->auth_type, "none") != 0)
      return 0;
   return 1;
}

static int server_agent_set_model_concurrency(const char *model, int limit)
{
   if (!model || !model[0] || limit <= 0)
      return 0;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return -1;
   for (int i = 0; i < cfg.concurrency_per_model_count; i++)
   {
      if (strcmp(cfg.concurrency_per_model[i].key, model) == 0)
      {
         cfg.concurrency_per_model[i].limit = limit;
         return config_save(&cfg);
      }
   }
   if (cfg.concurrency_per_model_count >= CONFIG_CONCURRENCY_MAX_ENTRIES)
      return -1;
   config_concurrency_entry_t *entry =
       &cfg.concurrency_per_model[cfg.concurrency_per_model_count++];
   snprintf(entry->key, sizeof(entry->key), "%s", model);
   entry->limit = limit;
   return config_save(&cfg);
}

static int server_agent_model_still_configured(const agent_config_t *cfg, const char *model)
{
   if (!cfg || !model || !model[0])
      return 0;
   for (int i = 0; i < cfg->agent_count; i++)
      if (strcmp(cfg->agents[i].model, model) == 0)
         return 1;
   return 0;
}

static int server_agent_clear_model_concurrency_if_unused(const agent_config_t *agents,
                                                          const char *model)
{
   if (!model || !model[0] || server_agent_model_still_configured(agents, model))
      return 0;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return -1;
   for (int i = 0; i < cfg.concurrency_per_model_count; i++)
   {
      if (strcmp(cfg.concurrency_per_model[i].key, model) != 0)
         continue;
      memmove(&cfg.concurrency_per_model[i], &cfg.concurrency_per_model[i + 1],
              (size_t)(cfg.concurrency_per_model_count - i - 1) *
                  sizeof(cfg.concurrency_per_model[0]));
      cfg.concurrency_per_model_count--;
      return config_save(&cfg);
   }
   return 0;
}

static void server_agent_ensure_fallback(agent_config_t *cfg, const char *name)
{
   if (!name || !name[0])
      return;
   for (int i = 0; i < cfg->fallback_count; i++)
      if (strcmp(cfg->fallback_chain[i], name) == 0)
         return;
   if (cfg->fallback_count >= MAX_FALLBACK)
      cfg->fallback_count = MAX_FALLBACK - 1;
   memmove(&cfg->fallback_chain[1], &cfg->fallback_chain[0],
           (size_t)cfg->fallback_count * sizeof(cfg->fallback_chain[0]));
   snprintf(cfg->fallback_chain[0], sizeof(cfg->fallback_chain[0]), "%s", name);
   cfg->fallback_count++;
}

static void server_agent_remove_fallback(agent_config_t *cfg, const char *name)
{
   if (!name || !name[0])
      return;
   for (int i = 0; i < cfg->fallback_count; i++)
   {
      if (strcmp(cfg->fallback_chain[i], name) != 0)
         continue;
      memmove(&cfg->fallback_chain[i], &cfg->fallback_chain[i + 1],
              (size_t)(cfg->fallback_count - i - 1) * sizeof(cfg->fallback_chain[0]));
      cfg->fallback_count--;
      i--;
   }
}

static cJSON *server_agent_to_json(const agent_t *ag)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "name", ag->name);
   cJSON_AddStringToObject(obj, "endpoint", ag->endpoint);
   cJSON_AddStringToObject(obj, "model", ag->model);
   cJSON_AddStringToObject(obj, "auth_type", ag->auth_type);
   cJSON_AddStringToObject(obj, "provider", ag->provider);
   cJSON_AddNumberToObject(obj, "cost_tier", ag->cost_tier);
   cJSON_AddBoolToObject(obj, "enabled", ag->enabled);
   cJSON_AddBoolToObject(obj, "tools_enabled", ag->tools_enabled);
   cJSON_AddNumberToObject(obj, "max_turns", ag->max_turns);
   cJSON_AddNumberToObject(obj, "max_parallel", ag->max_parallel);
   if (ag->middleware.context_window > 0)
      cJSON_AddNumberToObject(obj, "context_window", ag->middleware.context_window);
   cJSON *roles = cJSON_CreateArray();
   for (int j = 0; j < ag->role_count; j++)
      cJSON_AddItemToArray(roles, cJSON_CreateString(ag->roles[j]));
   cJSON_AddItemToObject(obj, "roles", roles);
   return obj;
}

static int server_agent_args(cJSON *req, char **argv, int max)
{
   cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "args");
   if (!cJSON_IsArray(args))
      return 0;
   int n = cJSON_GetArraySize(args);
   if (n > max)
      n = max;
   for (int i = 0; i < n; i++)
   {
      cJSON *a = cJSON_GetArrayItem(args, i);
      argv[i] = (char *)(cJSON_IsString(a) ? a->valuestring : "");
   }
   return n;
}

char *server_agent_list_json(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      memset(&cfg, 0, sizeof(cfg));
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < cfg.agent_count; i++)
      cJSON_AddItemToArray(arr, server_agent_to_json(&cfg.agents[i]));
   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

int handle_agent_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      memset(&cfg, 0, sizeof(cfg));
   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < cfg.agent_count; i++)
      cJSON_AddItemToArray(arr, server_agent_to_json(&cfg.agents[i]));
   cJSON_AddItemToObject(resp, "agents", arr);
   return server_send_ok(conn, resp);
}

int handle_agent_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   static const char *bool_flags[] = {"tools",   "tools-enabled", "no-tools",
                                      "default", "disabled",      NULL};
   char *argv[SERVER_AGENT_MAX_ARGS];
   int argc = server_agent_args(req, argv, (int)(sizeof(argv) / sizeof(argv[0])));
   if (argc < 3)
      return server_send_error(conn, "usage: agent add <name> <endpoint> <model>", NULL);

   opt_parsed_t opts;
   opt_parse(argc - 3, argv + 3, bool_flags, &opts);

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      memset(&cfg, 0, sizeof(cfg));

   char old_model[MAX_MODEL_LEN] = {0};
   agent_t *ag = agent_find(&cfg, argv[0]);
   if (!ag)
   {
      if (cfg.agent_count >= MAX_AGENTS)
         return server_send_error(conn, "maximum number of agents reached", NULL);
      ag = &cfg.agents[cfg.agent_count++];
   }
   else
      snprintf(old_model, sizeof(old_model), "%s", ag->model);
   memset(ag, 0, sizeof(*ag));

   snprintf(ag->name, sizeof(ag->name), "%s", argv[0]);
   snprintf(ag->endpoint, sizeof(ag->endpoint), "%s", argv[1]);
   snprintf(ag->model, sizeof(ag->model), "%s", argv[2]);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = AGENT_DEFAULT_TIMEOUT_MS;
   ag->max_turns = -1;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   ag->enabled = opt_has(&opts, "disabled") ? 0 : 1;

   /* A literal --key is a secret: vault it (encrypted at rest), never persist it
    * in agents.json. A $VAR reference is not a secret — keep it as a reference
    * that resolves from the environment at run time. */
   const char *key = opt_get(&opts, "key");
   if (key && key[0])
   {
      if (key[0] == '$')
      {
         /* An env reference is not a secret: store it UNEXPANDED so agents.json
          * holds "$VAR", not the resolved value. agent_load_config expands it
          * from the environment at run time. Expanding here would serialize the
          * plaintext key to disk — the exact leak the literal branch avoids.
          * api_key_disk is set explicitly too, so the on-disk form is correct
          * regardless of how the agent is re-saved later. */
         snprintf(ag->api_key, sizeof(ag->api_key), "%s", key);
         snprintf(ag->api_key_disk, sizeof(ag->api_key_disk), "%s", key);
      }
      else
      {
         /* A local/webchat caller with a per-user vault gets a dual-access entry
          * (requires the vault unlocked); a remote (TCP) caller has no per-user
          * principal, so the secret lands in the server-owned vault, which the
          * server can decrypt autonomously. On any failure we REFUSE rather than
          * write the secret to agents.json in plaintext. */
         const char *principal = (conn && conn->vault_principal[0]) ? conn->vault_principal : NULL;
         vault_status_t vst =
             principal
                 ? vault_service_set(principal, ag->name, VAULT_API_KEY_CRED, key, (long)time(NULL))
                 : vault_service_set_server(ag->name, VAULT_API_KEY_CRED, key);
         if (vst != VAULT_OK)
         {
            if (vst == VAULT_ERR_LOCKED)
               return server_send_error(
                   conn, "vault locked: run `aimee vault unlock` before adding a key", NULL);
            return server_send_error(conn, "could not store credential in the vault", NULL);
         }
         ag->api_key[0] = '\0';      /* the secret lives only in the vault */
         ag->api_key_disk[0] = '\0'; /* and nothing goes to agents.json */
      }
   }
   const char *auth_cmd = opt_get(&opts, "auth-cmd");
   if (auth_cmd && auth_cmd[0])
      snprintf(ag->auth_cmd, sizeof(ag->auth_cmd), "%s", auth_cmd);
   const char *auth_type = opt_get(&opts, "auth-type");
   if (auth_type && auth_type[0])
      snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", auth_type);
   const char *provider = opt_get(&opts, "provider");
   if (provider && provider[0])
      snprintf(ag->provider, sizeof(ag->provider), "%s", provider);

   /* `--provider codex` is a convenience alias for the Codex (ChatGPT OAuth)
    * adapter, whose provider is "chatgpt" (the responses-wire delegate driver)
    * and whose auth is codex-oauth. Without this, a literal provider "codex"
    * routes to a chat-completions path the codex backend rejects. */
   if (strcmp(ag->provider, "codex") == 0)
   {
      snprintf(ag->provider, sizeof(ag->provider), "chatgpt");
      if (!auth_type || !auth_type[0])
         snprintf(ag->auth_type, sizeof(ag->auth_type), "codex-oauth");
   }

   /* `--provider claude`/`claude-code` configures the standard `claude` CLI run
    * over tmux (login-based, not an API key) — the same tmux-cli backend
    * `agent setup claude` produces, but reachable from a thin client (where
    * `agent setup` has no local server). Without this it would be stored as a
    * generic HTTP agent pointing at a bogus endpoint. The tmux session runs on
    * the client over the reverse channel for a detached workspace; the `model`
    * arg becomes `claude --model <model>`. */
   if (strcmp(ag->provider, "claude") == 0 || strcmp(ag->provider, "claude-code") == 0)
   {
      int is_code = strcmp(ag->provider, "claude-code") == 0;
      snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
      snprintf(ag->cli_kind, sizeof(ag->cli_kind), "%s", is_code ? "claude-code" : "claude");
      if (ag->model[0])
         snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "claude --model %s", ag->model);
      else
         snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "claude");
      ag->session_reuse = 1;
      ag->endpoint[0] = '\0'; /* tmux CLI has no HTTP endpoint */
   }

   const char *roles = opt_get(&opts, "roles");
   if (roles && roles[0])
      server_agent_set_roles_csv(ag, roles);
   else
      server_agent_set_roles_csv(ag, "summarize,format,draft");

   const char *exec_roles = opt_get(&opts, "exec-roles");
   if (exec_roles && exec_roles[0])
      server_agent_set_exec_roles_csv(ag, exec_roles);

   ag->cost_tier = opt_get_int(&opts, "cost-tier", 0);
   ag->tools_enabled = opt_has(&opts, "tools") || opt_has(&opts, "tools-enabled");
   if (opt_has(&opts, "no-tools"))
      ag->tools_enabled = 0;
   ag->max_turns = opt_get_int(&opts, "max-turns", ag->max_turns);
   ag->max_parallel = opt_get_int(&opts, "max-parallel", ag->max_parallel);
   ag->max_tokens = opt_get_int(&opts, "max-tokens", ag->max_tokens);
   ag->timeout_ms = opt_get_int(&opts, "timeout-ms", opt_get_int(&opts, "timeout", ag->timeout_ms));
   ag->middleware.context_window =
       opt_get_int(&opts, "ctx", opt_get_int(&opts, "context-window", 0));

   if (opt_has(&opts, "default") || cfg.agent_count == 1 || !cfg.default_agent[0])
      snprintf(cfg.default_agent, sizeof(cfg.default_agent), "%s", ag->name);

   if (agent_save_config(&cfg) != 0)
      return server_send_error(conn, "could not save agents.json", NULL);
   if (ag->max_parallel > 0)
      (void)server_agent_set_model_concurrency(ag->model, ag->max_parallel);
   if (old_model[0] && strcmp(old_model, ag->model) != 0)
      (void)server_agent_clear_model_concurrency_if_unused(&cfg, old_model);

   cJSON *resp = server_agent_to_json(ag);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

int handle_agent_local(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   static const char *bool_flags[] = {"default", "no-probe", "no-tools", "no-fallback", NULL};
   char *argv[SERVER_AGENT_MAX_ARGS];
   int argc = server_agent_args(req, argv, (int)(sizeof(argv) / sizeof(argv[0])));
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *name = opt_get(&opts, "name");
   const char *endpoint_arg = opt_get(&opts, "endpoint");
   const char *model_arg = opt_get(&opts, "model");
   const char *roles_arg = opt_get(&opts, "roles");
   const char *exec_roles_arg = opt_get(&opts, "exec-roles");
   const char *p0 = opt_pos(&opts, 0);
   const char *p1 = opt_pos(&opts, 1);
   if (!endpoint_arg && p0 && server_agent_looks_endpoint(p0))
   {
      endpoint_arg = p0;
      if (!name)
         name = "local";
   }
   else
   {
      if (!name && p0)
         name = p0;
      if (!endpoint_arg && p1)
         endpoint_arg = p1;
   }
   if (!name || !name[0])
      name = "local";
   if (!endpoint_arg || !endpoint_arg[0])
      return server_send_error(conn, "agent.local requires endpoint", NULL);

   char endpoint[MAX_ENDPOINT_LEN];
   server_agent_normalize_endpoint(endpoint_arg, endpoint, sizeof(endpoint));

   char model[MAX_MODEL_LEN] = {0};
   if (model_arg && model_arg[0])
      snprintf(model, sizeof(model), "%s", model_arg);
   int slots = opt_get_int(&opts, "slots", 0);
   int context_window = opt_get_int(&opts, "ctx", 0);
   if (!context_window)
      context_window = opt_get_int(&opts, "context-window", 0);

   int detected_slots = 0, detected_ctx = 0, model_available = model[0] ? 0 : 1;
   char model_probe_msg[256] = {0}, slot_probe_msg[256] = {0};
   if (!opt_has(&opts, "no-probe"))
   {
      server_agent_http_ensure();
      char detected_model[MAX_MODEL_LEN] = {0};
      /* `agent local` targets keyless local endpoints (ollama/llama.cpp) and
       * parses no --key, so there is no bearer to present at probe time. */
      (void)server_agent_probe_models(endpoint, NULL, model[0] ? model : NULL, detected_model,
                                      sizeof(detected_model), &model_available, model_probe_msg,
                                      sizeof(model_probe_msg));
      if (!model[0] && detected_model[0])
         snprintf(model, sizeof(model), "%s", detected_model);
      (void)server_agent_probe_slots(endpoint, NULL, &detected_slots, &detected_ctx, slot_probe_msg,
                                     sizeof(slot_probe_msg));
   }
   if (!model[0])
      return server_send_error(conn, "agent.local could not determine model; pass --model", NULL);
   if (slots <= 0 && detected_slots > 0)
      slots = detected_slots;
   if (slots <= 0)
      slots = 1;
   if (context_window <= 0 && detected_ctx > 0)
      context_window = detected_ctx;

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      memset(&cfg, 0, sizeof(cfg));
   char old_model[MAX_MODEL_LEN] = {0};
   agent_t *ag = agent_find(&cfg, name);
   if (!ag)
   {
      if (cfg.agent_count >= MAX_AGENTS)
         return server_send_error(conn, "maximum number of agents reached", NULL);
      ag = &cfg.agents[cfg.agent_count++];
   }
   else
      snprintf(old_model, sizeof(old_model), "%s", ag->model);
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, sizeof(ag->name), "%s", name);
   snprintf(ag->endpoint, sizeof(ag->endpoint), "%s", endpoint);
   snprintf(ag->model, sizeof(ag->model), "%s", model);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "none");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->cost_tier = opt_get_int(&opts, "cost-tier", 0);
   ag->max_tokens = opt_get_int(&opts, "max-tokens", AGENT_DEFAULT_MAX_TOKENS);
   ag->timeout_ms = opt_get_int(&opts, "timeout-ms", opt_get_int(&opts, "timeout", 300000));
   ag->enabled = 1;
   ag->tools_enabled = opt_has(&opts, "no-tools") ? 0 : 1;
   ag->max_turns = opt_get_int(&opts, "max-turns", -1);
   ag->max_parallel = slots;
   ag->middleware.context_window = context_window;
   if (roles_arg && roles_arg[0])
      server_agent_set_roles_csv(ag, roles_arg);
   else
      server_agent_default_roles(ag);
   if (exec_roles_arg && exec_roles_arg[0])
      server_agent_set_exec_roles_csv(ag, exec_roles_arg);
   if (opt_has(&opts, "default") || cfg.agent_count == 1 || !cfg.default_agent[0])
      snprintf(cfg.default_agent, sizeof(cfg.default_agent), "%s", ag->name);
   if (!opt_has(&opts, "no-fallback"))
      server_agent_ensure_fallback(&cfg, ag->name);

   if (agent_save_config(&cfg) != 0)
      return server_send_error(conn, "could not save agents.json", NULL);
   if (server_agent_set_model_concurrency(ag->model, ag->max_parallel) != 0)
      return server_send_error(conn, "could not update per-model concurrency", NULL);
   if (old_model[0] && strcmp(old_model, ag->model) != 0)
      (void)server_agent_clear_model_concurrency_if_unused(&cfg, old_model);

   cJSON *resp = server_agent_to_json(ag);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "model_available", model_available ? 1 : 0);
   if (model_probe_msg[0])
      cJSON_AddStringToObject(resp, "model_probe", model_probe_msg);
   if (slot_probe_msg[0])
      cJSON_AddStringToObject(resp, "slot_probe", slot_probe_msg);
   return server_send_ok(conn, resp);
}

int handle_agent_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[SERVER_AGENT_MAX_ARGS];
   int argc = server_agent_args(req, argv, (int)(sizeof(argv) / sizeof(argv[0])));
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "agent.remove requires name", NULL);

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      return server_send_error(conn, "agents.json not found or invalid", NULL);

   int found = -1;
   for (int i = 0; i < cfg.agent_count; i++)
   {
      if (strcmp(cfg.agents[i].name, argv[0]) == 0)
      {
         found = i;
         break;
      }
   }
   if (found < 0)
      return server_send_error(conn, "agent not found", NULL);

   char removed[MAX_AGENT_NAME];
   char removed_model[MAX_MODEL_LEN];
   snprintf(removed, sizeof(removed), "%s", cfg.agents[found].name);
   snprintf(removed_model, sizeof(removed_model), "%s", cfg.agents[found].model);
   memmove(&cfg.agents[found], &cfg.agents[found + 1],
           (size_t)(cfg.agent_count - found - 1) * sizeof(cfg.agents[0]));
   cfg.agent_count--;
   server_agent_remove_fallback(&cfg, removed);
   if (strcmp(cfg.default_agent, removed) == 0)
      snprintf(cfg.default_agent, sizeof(cfg.default_agent), "%s",
               cfg.agent_count > 0 ? cfg.agents[0].name : "");

   if (agent_save_config(&cfg) != 0)
      return server_send_error(conn, "could not save agents.json", NULL);
   (void)server_agent_clear_model_concurrency_if_unused(&cfg, removed_model);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "name", removed);
   cJSON_AddBoolToObject(resp, "removed", 1);
   return server_send_ok(conn, resp);
}

static int handle_agent_set_enabled(server_ctx_t *ctx, server_conn_t *conn, cJSON *req, int enabled)
{
   (void)ctx;
   char *argv[SERVER_AGENT_MAX_ARGS];
   int argc = server_agent_args(req, argv, (int)(sizeof(argv) / sizeof(argv[0])));
   if (argc < 1 || !argv[0][0])
      return server_send_error(
          conn, enabled ? "agent.enable requires name" : "agent.disable requires name", NULL);

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      return server_send_error(conn, "agents.json not found or invalid", NULL);
   agent_t *ag = agent_find(&cfg, argv[0]);
   if (!ag)
      return server_send_error(conn, "agent not found", NULL);
   ag->enabled = enabled ? 1 : 0;

   if (agent_save_config(&cfg) != 0)
      return server_send_error(conn, "could not save agents.json", NULL);

   cJSON *resp = server_agent_to_json(ag);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

int handle_agent_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_agent_set_enabled(ctx, conn, req, 1);
}

int handle_agent_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_agent_set_enabled(ctx, conn, req, 0);
}

int handle_agent_probe(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   static const char *bool_flags[] = {"no-run", NULL};
   char *argv[SERVER_AGENT_MAX_ARGS];
   int argc = server_agent_args(req, argv, (int)(sizeof(argv) / sizeof(argv[0])));
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   const char *name = opt_pos(&opts, 0);
   if (!name || !name[0])
      return server_send_error(conn, "agent.probe requires name", NULL);

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      return server_send_error(conn, "agents.json not found or invalid", NULL);
   agent_t *ag = agent_find(&cfg, name);
   if (!ag)
      return server_send_error(conn, "agent not found", NULL);

   /* Present the agent's own credentials on the introspection GETs — hosted
    * providers (e.g. MiMo) return 401 on /models without a bearer even though
    * chat/completions works, producing a spurious "models: warn (401)". */
   char auth_header[MAX_API_KEY_LEN + 32] = {0};
   if (agent_resolve_auth(ag, auth_header, sizeof(auth_header)) != 0)
      auth_header[0] = '\0';
   const char *probe_auth = auth_header[0] ? auth_header : NULL;

   server_agent_http_ensure();
   char model_found[MAX_MODEL_LEN] = {0}, model_msg[256] = {0}, slots_msg[256] = {0};
   int model_available = 0, slots = 0, context_window = 0;
   int models_status = server_agent_probe_models(ag->endpoint, probe_auth, ag->model, model_found,
                                                 sizeof(model_found), &model_available, model_msg,
                                                 sizeof(model_msg));
   const char *slots_source = "probe";
   int slots_probe_skipped = 0;
   int slots_status = 0;
   if (server_agent_should_probe_slots(ag))
      slots_status = server_agent_probe_slots(ag->endpoint, probe_auth, &slots, &context_window,
                                              slots_msg, sizeof(slots_msg));
   else
   {
      slots = ag->max_parallel;
      context_window = ag->middleware.context_window;
      slots_source = "config";
      slots_probe_skipped = 1;
   }
   int run_ok = 0, latency_ms = 0;
   char run_msg[512] = {0};
   if (!opt_has(&opts, "no-run"))
   {
      agent_result_t result;
      int rc = agent_execute(ag, NULL, "Respond with ok.", 16, 0.0, &result);
      run_ok = (rc == 0);
      latency_ms = result.latency_ms;
      snprintf(run_msg, sizeof(run_msg), "%s",
               run_ok ? (result.response ? result.response : "") : result.error);
      free(result.response);
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "name", ag->name);
   cJSON_AddStringToObject(resp, "endpoint", ag->endpoint);
   cJSON_AddStringToObject(resp, "model", ag->model);
   cJSON_AddNumberToObject(resp, "models_status", models_status);
   cJSON_AddBoolToObject(resp, "model_available", model_available ? 1 : 0);
   cJSON_AddNumberToObject(resp, "slots_status", slots_status);
   cJSON_AddNumberToObject(resp, "detected_slots", slots);
   cJSON_AddStringToObject(resp, "slots_source", slots_source);
   if (slots_probe_skipped)
      cJSON_AddBoolToObject(resp, "slots_probe_skipped", 1);
   if (context_window > 0)
      cJSON_AddNumberToObject(resp, "detected_context_window", context_window);
   if (!opt_has(&opts, "no-run"))
   {
      cJSON_AddBoolToObject(resp, "execution_ok", run_ok ? 1 : 0);
      cJSON_AddNumberToObject(resp, "latency_ms", latency_ms);
      cJSON_AddStringToObject(resp, "execution_message", run_msg);
   }
   if (model_msg[0])
      cJSON_AddStringToObject(resp, "model_probe", model_msg);
   if (slots_msg[0])
      cJSON_AddStringToObject(resp, "slot_probe", slots_msg);
   return server_send_ok(conn, resp);
}

/* --- agent.setup / agent.setup_poll: codex-oauth device flow --- */

#define SAGENT_CODEX_OAUTH_CLIENT_ID  "app_EMoamEEZ73f0CkXaXp7hrann"
#define SAGENT_CODEX_USERCODE_URL     "https://auth.openai.com/api/accounts/deviceauth/usercode"
#define SAGENT_CODEX_DEVICETOKEN_URL  "https://auth.openai.com/api/accounts/deviceauth/token"
#define SAGENT_CODEX_TOKEN_URL        "https://auth.openai.com/oauth/token"
#define SAGENT_CODEX_VERIFY_URL       "https://auth.openai.com/codex/device"
#define SAGENT_CODEX_REDIRECT_URI     "https://auth.openai.com/deviceauth/callback"
#define SAGENT_CODEX_DEFAULT_INTERVAL 5
#define SAGENT_CODEX_DEFAULT_EXPIRES  900

static int sagent_b64url_char(char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '-')
      return 62;
   if (c == '_')
      return 63;
   return -1;
}

static void sagent_jwt_account_id(const char *jwt, char *buf, size_t buf_len)
{
   buf[0] = '\0';
   if (!jwt)
      return;
   const char *p1 = strchr(jwt, '.');
   if (!p1)
      return;
   p1++;
   const char *p2 = strchr(p1, '.');
   size_t seg_len = p2 ? (size_t)(p2 - p1) : strlen(p1);

   char decoded[4096];
   size_t o = 0;
   unsigned int b = 0;
   int bits = 0;
   for (size_t i = 0; i < seg_len && o < sizeof(decoded) - 1; i++)
   {
      int v = sagent_b64url_char(p1[i]);
      if (v < 0)
         continue;
      b = (b << 6) | (unsigned int)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         decoded[o++] = (char)((b >> bits) & 0xFF);
      }
   }
   decoded[o] = '\0';

   cJSON *claims = cJSON_Parse(decoded);
   if (!claims)
      return;
   cJSON *aid = cJSON_GetObjectItem(claims, "chatgpt_account_id");
   if (!aid || !cJSON_IsString(aid))
   {
      cJSON *ns = cJSON_GetObjectItem(claims, "https://api.openai.com/auth");
      if (ns && cJSON_IsObject(ns))
         aid = cJSON_GetObjectItem(ns, "chatgpt_account_id");
   }
   if (aid && cJSON_IsString(aid))
      snprintf(buf, buf_len, "%s", aid->valuestring);
   cJSON_Delete(claims);
}

static const char *sagent_provider_cli_roles[] = {"code",     "review", "explain",
                                                  "refactor", "draft",  "execute"};
static const char *sagent_codex_oauth_roles[] = {"code",     "review",  "explain",   "refactor",
                                                 "draft",    "execute", "summarize", "format",
                                                 "diagnose", "validate"};
static const char *sagent_mistral_plan_roles[] = {"code",     "review",  "explain",   "refactor",
                                                  "draft",    "execute", "summarize", "format",
                                                  "diagnose", "validate"};

static void sagent_configure_provider_cli_agent_with_roles(agent_t *ag, const char *name,
                                                           const char *provider,
                                                           const char *cli_kind,
                                                           const char *cli_cmd, int cost_tier,
                                                           const char *const *roles, int role_count)
{
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, MAX_AGENT_NAME, "%s", name);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "%s", provider);
   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(ag->cli_kind, sizeof(ag->cli_kind), "%s", cli_kind);
   snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", cli_cmd);
   ag->cost_tier = cost_tier;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 600000;
   ag->enabled = 1;
   ag->tools_enabled = 1;
   ag->max_turns = -1;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   for (int i = 0; i < role_count && ag->role_count < MAX_AGENT_ROLES; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);
}

static void sagent_configure_provider_cli_agent(agent_t *ag, const char *name, const char *provider,
                                                const char *cli_kind, const char *cli_cmd,
                                                int cost_tier)
{
   sagent_configure_provider_cli_agent_with_roles(
       ag, name, provider, cli_kind, cli_cmd, cost_tier, sagent_provider_cli_roles,
       (int)(sizeof(sagent_provider_cli_roles) / sizeof(sagent_provider_cli_roles[0])));
}

static void sagent_configure_tmux_cli_agent(agent_t *ag, const char *name, const char *provider,
                                            const char *cli_kind, const char *cli_cmd,
                                            int cost_tier)
{
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, MAX_AGENT_NAME, "%s", name);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "none");
   snprintf(ag->provider, sizeof(ag->provider), "%s", provider);
   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   snprintf(ag->cli_kind, sizeof(ag->cli_kind), "%s", cli_kind);
   snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", cli_cmd);
   ag->cost_tier = cost_tier;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 600000;
   ag->enabled = 1;
   ag->tools_enabled = 1;
   ag->max_turns = -1;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   ag->session_reuse = 1;

   ag->role_count = 0;
   for (int i = 0;
        i < (int)(sizeof(sagent_provider_cli_roles) / sizeof(sagent_provider_cli_roles[0])) &&
        ag->role_count < MAX_AGENT_ROLES;
        i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", sagent_provider_cli_roles[i]);
}

static void
sagent_configure_direct_adapter_agent_with_roles(agent_t *ag, const agent_adapter_t *adapter,
                                                 const char *name, int cost_tier, int timeout_ms,
                                                 const char *const *roles, int role_count)
{
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, MAX_AGENT_NAME, "%s", name);
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", adapter->default_endpoint);
   snprintf(ag->model, MAX_MODEL_LEN, "%s", adapter->default_model);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", adapter->auth_type);
   snprintf(ag->provider, sizeof(ag->provider), "%s", adapter->provider);
   ag->cost_tier = cost_tier;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = timeout_ms;
   ag->enabled = 1;
   ag->tools_enabled = 1;
   ag->max_turns = -1;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   for (int i = 0; i < role_count && ag->role_count < MAX_AGENT_ROLES; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);
}

static int sagent_setup_provider_cli(server_conn_t *conn, const char *provider)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      memset(&cfg, 0, sizeof(cfg));

   int is_mistral_plan = strcmp(provider, "mistral-plan") == 0;
   int is_codex_cli = strcmp(provider, "codex-cli") == 0;
   int is_claude = strcmp(provider, "claude") == 0;
   int is_claude_code = strcmp(provider, "claude-code") == 0;
   int is_claude_tmux = is_claude || is_claude_code;
   int is_gemini_cli = strcmp(provider, "gemini-cli") == 0;
   int is_mistral_cli = strcmp(provider, "mistral-cli") == 0;
   const char *agent_name = is_mistral_plan ? "mistral-plan" : provider;
   const char *agent_provider = is_mistral_plan  ? "mistral"
                                : is_codex_cli   ? "codex"
                                : is_claude_code ? "claude-code"
                                : is_gemini_cli  ? "gemini"
                                : is_mistral_cli ? "mistral"
                                                 : provider;
   const char *cli_kind = is_mistral_plan  ? "mistral-plan"
                          : is_codex_cli   ? "codex"
                          : is_claude_code ? "claude-code"
                          : is_gemini_cli  ? "gemini"
                          : is_mistral_cli ? "mistral"
                                           : provider;
   const int is_native_provider_cli = is_mistral_plan || is_gemini_cli || is_mistral_cli;
   const char *cli_cmd = is_native_provider_cli ? ""
                         : is_codex_cli         ? "codex"
                         : is_claude_tmux       ? "claude"
                                                : provider;
   const char *native_message =
       is_gemini_cli ? "Uses GEMINI_API_KEY or GOOGLE_API_KEY; no Gemini CLI is launched."
       : is_mistral_plan
           ? "Uses MISTRAL_API_KEY or ~/.vibe/.env with a Vibe-compatible Mistral request shape; "
             "no Vibe CLI is launched."
           : "Uses MISTRAL_API_KEY or ~/.vibe/.env; no Mistral CLI is launched.";
   const char *cli_message = "Uses the installed provider CLI; run the provider login if needed.";
   if (is_codex_cli)
      cli_message = "Uses the installed `codex` CLI.";
   else if (is_claude_code)
      cli_message = "Uses the installed `claude` CLI in a reusable tmux session.";
   int cost_tier = (is_mistral_plan || is_codex_cli || is_gemini_cli || is_mistral_cli) ? 0 : 1;

   agent_t *ag = agent_find(&cfg, agent_name);
   int is_new = 0;
   if (!ag)
   {
      if (cfg.agent_count >= MAX_AGENTS)
         return server_send_error(conn, "maximum number of agents reached", NULL);
      ag = &cfg.agents[cfg.agent_count];
      cfg.agent_count++;
      is_new = 1;
   }

   if (is_claude_tmux)
      sagent_configure_tmux_cli_agent(ag, agent_name, agent_provider, cli_kind, cli_cmd, cost_tier);
   else if (is_mistral_plan)
      sagent_configure_provider_cli_agent_with_roles(
          ag, agent_name, agent_provider, cli_kind, cli_cmd, cost_tier, sagent_mistral_plan_roles,
          (int)(sizeof(sagent_mistral_plan_roles) / sizeof(sagent_mistral_plan_roles[0])));
   else
      sagent_configure_provider_cli_agent(ag, agent_name, agent_provider, cli_kind, cli_cmd,
                                          cost_tier);

   if (cfg.agent_count == 1 || !cfg.default_agent[0])
      snprintf(cfg.default_agent, MAX_AGENT_NAME, "%s", agent_name);

   if (agent_save_config(&cfg) != 0)
      return server_send_error(conn, "failed to save agent config", NULL);

   char msg[256];
   if (is_claude_tmux)
      snprintf(msg, sizeof(msg),
               "Agent '%s' %s (tmux CLI). Uses tmux with the installed `claude` CLI.", agent_name,
               is_new ? "created" : "updated");
   else if (is_native_provider_cli)
      snprintf(msg, sizeof(msg), "Agent '%s' %s (native %s adapter). %s", agent_name,
               is_new ? "created" : "updated", agent_provider, native_message);
   else
      snprintf(msg, sizeof(msg), "Agent '%s' %s (provider CLI). %s", agent_name,
               is_new ? "created" : "updated", cli_message);

   cJSON *out = jo_ok();
   cJSON_AddBoolToObject(out, "complete", 1);
   cJSON_AddStringToObject(out, "provider", provider);
   cJSON_AddStringToObject(out, "agent", agent_name);
   cJSON_AddStringToObject(out, "message", msg);
   cJSON_AddBoolToObject(out, "is_new", is_new);
   return server_send_ok(conn, out);
}

/* Step 1: request device code from OpenAI, return verify_url + user_code to client */
int handle_agent_setup(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *j_provider = cJSON_GetObjectItemCaseSensitive(req, "provider");
   const char *provider = cJSON_IsString(j_provider) ? j_provider->valuestring : NULL;

   if (provider && (strcmp(provider, "codex-cli") == 0 || strcmp(provider, "claude") == 0 ||
                    strcmp(provider, "claude-code") == 0 || strcmp(provider, "gemini-cli") == 0 ||
                    strcmp(provider, "mistral-cli") == 0 || strcmp(provider, "mistral-plan") == 0))
      return sagent_setup_provider_cli(conn, provider);

   if (!provider || (strcmp(provider, "codex") != 0 && strcmp(provider, "codex-oauth") != 0))
      return server_send_error(conn,
                               "agent.setup: unsupported provider (expected: codex, codex-oauth, "
                               "codex-cli, claude, claude-code, gemini-cli, mistral-cli, "
                               "mistral-plan)",
                               NULL);

   server_agent_http_ensure();

   char post_body[512];
   snprintf(post_body, sizeof(post_body), "{\"client_id\":\"%s\"}", SAGENT_CODEX_OAUTH_CLIENT_ID);

   char *resp_body = NULL;
   int status =
       agent_http_post(SAGENT_CODEX_USERCODE_URL, NULL, post_body, &resp_body, 15000, NULL);
   if (status < 200 || status >= 300 || !resp_body)
   {
      free(resp_body);
      char errmsg[128];
      snprintf(errmsg, sizeof(errmsg), "failed to request device code (HTTP %d)", status);
      return server_send_error(conn, errmsg, NULL);
   }

   cJSON *dev = cJSON_Parse(resp_body);
   free(resp_body);
   if (!dev)
      return server_send_error(conn, "failed to parse device code response", NULL);

   cJSON *j_device_auth_id = cJSON_GetObjectItem(dev, "device_auth_id");
   cJSON *j_user_code = cJSON_GetObjectItem(dev, "user_code");
   cJSON *j_interval = cJSON_GetObjectItem(dev, "interval");

   if (!j_device_auth_id || !cJSON_IsString(j_device_auth_id) || !j_user_code ||
       !cJSON_IsString(j_user_code))
   {
      cJSON_Delete(dev);
      return server_send_error(conn, "device code response missing required fields", NULL);
   }

   int interval = (j_interval && cJSON_IsNumber(j_interval)) ? j_interval->valueint
                                                             : SAGENT_CODEX_DEFAULT_INTERVAL;

   cJSON *out = jo_ok();
   cJSON_AddStringToObject(out, "provider", provider);
   cJSON_AddStringToObject(out, "verify_url", SAGENT_CODEX_VERIFY_URL);
   cJSON_AddStringToObject(out, "user_code", j_user_code->valuestring);
   cJSON_AddStringToObject(out, "device_auth_id", j_device_auth_id->valuestring);
   cJSON_AddNumberToObject(out, "interval", interval);
   cJSON_AddNumberToObject(out, "expires_in", SAGENT_CODEX_DEFAULT_EXPIRES);
   cJSON_Delete(dev);

   return server_send_ok(conn, out);
}

/* Step 2: poll until authorized, then exchange tokens and save config */
int handle_agent_setup_poll(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *j_daid = cJSON_GetObjectItemCaseSensitive(req, "device_auth_id");
   cJSON *j_ucode = cJSON_GetObjectItemCaseSensitive(req, "user_code");
   cJSON *j_interval = cJSON_GetObjectItemCaseSensitive(req, "interval");

   if (!cJSON_IsString(j_daid) || !cJSON_IsString(j_ucode))
      return server_send_error(conn, "agent.setup_poll: missing device_auth_id or user_code", NULL);

   const char *device_auth_id = j_daid->valuestring;
   const char *user_code = j_ucode->valuestring;
   int interval = (cJSON_IsNumber(j_interval) && j_interval->valueint > 0)
                      ? j_interval->valueint
                      : SAGENT_CODEX_DEFAULT_INTERVAL;

   server_agent_http_ensure();

   char poll_body[1024];
   snprintf(poll_body, sizeof(poll_body), "{\"device_auth_id\":\"%s\",\"user_code\":\"%s\"}",
            device_auth_id, user_code);

   int elapsed = 0, authorized = 0;
   char auth_code[1024] = {0};
   char code_verifier[256] = {0};
   char last_resp_snippet[512] = {0};
   int last_resp_status = 0;

   while (elapsed < SAGENT_CODEX_DEFAULT_EXPIRES)
   {
      sleep((unsigned)interval);
      elapsed += interval;

      char *poll_resp = NULL;
      int poll_status =
          agent_http_post(SAGENT_CODEX_DEVICETOKEN_URL, NULL, poll_body, &poll_resp, 15000, NULL);

      LOG_DEBUG("agent_setup_poll", "poll HTTP %d: %.256s", poll_status,
                poll_resp ? poll_resp : "(no body)");

      if (poll_status == 200 && poll_resp)
      {
         cJSON *pr = cJSON_Parse(poll_resp);
         if (pr)
         {
            cJSON *j_ac = cJSON_GetObjectItem(pr, "authorization_code");
            cJSON *j_cv = cJSON_GetObjectItem(pr, "code_verifier");
            if (j_ac && cJSON_IsString(j_ac))
            {
               snprintf(auth_code, sizeof(auth_code), "%s", j_ac->valuestring);
               if (j_cv && cJSON_IsString(j_cv))
                  snprintf(code_verifier, sizeof(code_verifier), "%s", j_cv->valuestring);
               authorized = 1;
            }
            else
            {
               /* 200 OK but no authorization_code — log for diagnosis */
               LOG_WARN("agent_setup_poll",
                        "HTTP 200 but no authorization_code in response: %.256s", poll_resp);
               last_resp_status = poll_status;
               snprintf(last_resp_snippet, sizeof(last_resp_snippet), "%.511s", poll_resp);
            }
            cJSON_Delete(pr);
         }
      }
      if (authorized)
      {
         free(poll_resp);
         break;
      }
      /* 400 = authorization_pending or slow_down — keep polling unless terminal error */
      if (poll_status == 400 && poll_resp)
      {
         cJSON *ep = cJSON_Parse(poll_resp);
         cJSON *je = ep ? cJSON_GetObjectItem(ep, "error") : NULL;
         const char *ec = (je && cJSON_IsString(je)) ? je->valuestring : "";
         int terminal = strcmp(ec, "access_denied") == 0 || strcmp(ec, "expired_token") == 0;
         if (!last_resp_status)
         {
            last_resp_status = poll_status;
            snprintf(last_resp_snippet, sizeof(last_resp_snippet), "%.511s", poll_resp);
         }
         cJSON_Delete(ep);
         free(poll_resp);
         if (terminal)
            break;
         continue;
      }
      if (poll_resp)
      {
         last_resp_status = poll_status;
         snprintf(last_resp_snippet, sizeof(last_resp_snippet), "%.511s", poll_resp);
      }
      free(poll_resp);
      /* 403/404/429 = transient — keep polling; anything else is unexpected, stop */
      if (poll_status != 403 && poll_status != 404 && poll_status != 429 && poll_status != 200)
         break;
   }

   if (!authorized || !auth_code[0])
   {
      char errmsg[640];
      if (last_resp_snippet[0])
         snprintf(errmsg, sizeof(errmsg),
                  "authorization timed out or was denied (last HTTP %d: %s)", last_resp_status,
                  last_resp_snippet);
      else
         snprintf(errmsg, sizeof(errmsg), "%s", "authorization timed out or was denied");
      return server_send_error(conn, errmsg, NULL);
   }

   /* Exchange authorization code for OAuth tokens */
   char token_body[2048];
   snprintf(token_body, sizeof(token_body),
            "grant_type=authorization_code"
            "&code=%s"
            "&redirect_uri=%s"
            "&client_id=%s"
            "&code_verifier=%s",
            auth_code, SAGENT_CODEX_REDIRECT_URI, SAGENT_CODEX_OAUTH_CLIENT_ID, code_verifier);

   char *token_resp = NULL;
   int token_status = agent_http_post_form(SAGENT_CODEX_TOKEN_URL, token_body, &token_resp, 15000);
   if (token_status != 200 || !token_resp)
   {
      free(token_resp);
      char errmsg[128];
      snprintf(errmsg, sizeof(errmsg), "token exchange failed (HTTP %d)", token_status);
      return server_send_error(conn, errmsg, NULL);
   }

   cJSON *tok = cJSON_Parse(token_resp);
   free(token_resp);
   if (!tok)
      return server_send_error(conn, "failed to parse token response", NULL);

   cJSON *j_id_token = cJSON_GetObjectItem(tok, "id_token");
   cJSON *j_access_token = cJSON_GetObjectItem(tok, "access_token");
   cJSON *j_refresh = cJSON_GetObjectItem(tok, "refresh_token");
   cJSON *j_tok_expires = cJSON_GetObjectItem(tok, "expires_in");

   /* Try to exchange id_token for an API key */
   const char *final_token = NULL;
   int tok_expires = 3600;
   cJSON *exch = NULL;

   if (j_id_token && cJSON_IsString(j_id_token))
   {
      char exchange_body[4096];
      snprintf(exchange_body, sizeof(exchange_body),
               "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Atoken-exchange"
               "&client_id=%s"
               "&requested_token=openai-api-key"
               "&subject_token=%s"
               "&subject_token_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Atoken-type%%3Aid_token",
               SAGENT_CODEX_OAUTH_CLIENT_ID, j_id_token->valuestring);

      char *exch_resp = NULL;
      int exch_status =
          agent_http_post_form(SAGENT_CODEX_TOKEN_URL, exchange_body, &exch_resp, 15000);
      if (exch_status == 200 && exch_resp)
      {
         exch = cJSON_Parse(exch_resp);
         if (exch)
         {
            cJSON *j_api_key = cJSON_GetObjectItem(exch, "access_token");
            cJSON *j_api_expires = cJSON_GetObjectItem(exch, "expires_in");
            if (j_api_key && cJSON_IsString(j_api_key))
            {
               final_token = j_api_key->valuestring;
               if (j_api_expires && cJSON_IsNumber(j_api_expires))
                  tok_expires = j_api_expires->valueint;
            }
         }
      }
      free(exch_resp);
   }

   if (!final_token)
   {
      if (!j_access_token || !cJSON_IsString(j_access_token))
      {
         cJSON_Delete(exch);
         cJSON_Delete(tok);
         return server_send_error(conn, "no usable token obtained from OAuth flow", NULL);
      }
      final_token = j_access_token->valuestring;
      if (j_tok_expires && cJSON_IsNumber(j_tok_expires))
         tok_expires = j_tok_expires->valueint;
   }

   char chatgpt_account_id[128] = {0};
   if (j_id_token && cJSON_IsString(j_id_token))
      sagent_jwt_account_id(j_id_token->valuestring, chatgpt_account_id,
                            sizeof(chatgpt_account_id));

   /* Write auth JSON to disk */
   cJSON *auth_json = cJSON_CreateObject();
   cJSON_AddStringToObject(auth_json, "access_token", final_token);
   if (j_refresh && cJSON_IsString(j_refresh))
      cJSON_AddStringToObject(auth_json, "refresh_token", j_refresh->valuestring);
   if (j_id_token && cJSON_IsString(j_id_token))
      cJSON_AddStringToObject(auth_json, "id_token", j_id_token->valuestring);
   cJSON_AddNumberToObject(auth_json, "expires_at", (double)(time(NULL) + tok_expires));
   cJSON_AddStringToObject(auth_json, "client_id", SAGENT_CODEX_OAUTH_CLIENT_ID);

   char *auth_str = cJSON_Print(auth_json);
   cJSON_Delete(auth_json);
   cJSON_Delete(exch);
   cJSON_Delete(tok);

   if (!auth_str)
      return server_send_error(conn, "failed to serialize auth tokens", NULL);

   char auth_path[MAX_PATH_LEN];
   snprintf(auth_path, sizeof(auth_path), "%s/codex-auth.json", config_default_dir());
   platform_mkdir_p(config_default_dir(), 0700);

   FILE *f = fopen(auth_path, "w");
   if (!f)
   {
      free(auth_str);
      return server_send_error(conn, "failed to write auth file", NULL);
   }
   fputs(auth_str, f);
   fputc('\n', f);
   fclose(f);
   chmod(auth_path, 0600);
   free(auth_str);

   /* Update agents.json */
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      memset(&cfg, 0, sizeof(cfg));

   agent_t *ag = agent_find(&cfg, "codex");
   int is_new = 0;
   if (!ag)
   {
      if (cfg.agent_count >= MAX_AGENTS)
         return server_send_error(conn, "maximum number of agents reached", NULL);
      ag = &cfg.agents[cfg.agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg.agent_count++;
      is_new = 1;
   }

   const agent_adapter_t *adapter = agent_adapter_for_name("codex");
   if (!adapter)
      return server_send_error(conn, "codex adapter is not registered", NULL);
   sagent_configure_direct_adapter_agent_with_roles(
       ag, adapter, "codex", 0, 600000, sagent_codex_oauth_roles,
       (int)(sizeof(sagent_codex_oauth_roles) / sizeof(sagent_codex_oauth_roles[0])));
   if (chatgpt_account_id[0])
      snprintf(ag->extra_headers, sizeof(ag->extra_headers),
               "originator: codex_cli_rs\nChatGPT-Account-ID: %s", chatgpt_account_id);
   else
      snprintf(ag->extra_headers, sizeof(ag->extra_headers), "originator: codex_cli_rs");

   if (cfg.agent_count == 1 || !cfg.default_agent[0])
      snprintf(cfg.default_agent, MAX_AGENT_NAME, "codex");

   agent_save_config(&cfg);

   char msg[512];
   snprintf(msg, sizeof(msg), "Agent 'codex' %s (direct Codex adapter). Tokens saved to: %s.",
            is_new ? "created" : "updated", auth_path);

   cJSON *out = jo_ok();
   cJSON_AddStringToObject(out, "agent", "codex");
   cJSON_AddStringToObject(out, "message", msg);
   cJSON_AddBoolToObject(out, "is_new", is_new);
   if (chatgpt_account_id[0])
      cJSON_AddStringToObject(out, "oauth_account", chatgpt_account_id);
   return server_send_ok(conn, out);
}
