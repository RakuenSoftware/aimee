/* cmd_agent.c: agent subcommand CLI (agent list/test/run/add/remove/setup/token) */
#include "aimee.h"
#include "util.h"
#include "db1.h"
#include "agent.h"
#include "agent_tunnel.h"
#include "commands.h"
#include "hardware_probe.h"
#include "kb_client.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* File-scope agent config, loaded once by cmd_agent before dispatch */
/* Non-static so cmd_agent_setup.c can reference it. */
agent_config_t s_agent_cfg;

static void ag_set_roles_csv(agent_t *ag, const char *csv)
{
   ag->role_count = 0;
   if (!csv || !csv[0])
      return;

   char buf[512];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *tok = strtok(buf, ",");
   while (tok && ag->role_count < MAX_AGENT_ROLES)
   {
      char *role = util_trim(tok);
      if (role[0])
         snprintf(ag->roles[ag->role_count++], sizeof(ag->roles[0]), "%s", role);
      tok = strtok(NULL, ",");
   }
}

static void ag_set_exec_roles_csv(agent_t *ag, const char *csv)
{
   ag->exec_role_count = 0;
   if (!csv || !csv[0])
      return;

   char buf[256];
   snprintf(buf, sizeof(buf), "%s", csv);
   char *tok = strtok(buf, ",");
   while (tok && ag->exec_role_count < MAX_EXEC_ROLES)
   {
      char *role = util_trim(tok);
      if (role[0])
         snprintf(ag->exec_roles[ag->exec_role_count++], sizeof(ag->exec_roles[0]), "%s", role);
      tok = strtok(NULL, ",");
   }
}

static void ag_set_default_delegate_roles(agent_t *ag)
{
   ag_set_roles_csv(ag,
                    "code,review,explain,refactor,draft,execute,summarize,format,reason,search");
}

static int ag_looks_like_endpoint(const char *s)
{
   if (!s || !s[0])
      return 0;
   return strstr(s, "://") != NULL || strchr(s, ':') != NULL || strchr(s, '.') != NULL ||
          strcmp(s, "localhost") == 0 || strcmp(s, "127.0.0.1") == 0;
}

static void ag_normalize_endpoint(const char *input, char *out, size_t out_len)
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

static const char *ag_canonical_local_provider(const char *provider)
{
   if (!provider || !provider[0])
      return "openai";
   if (strcmp(provider, "llama-eval") == 0)
      return "openai";
   return provider;
}

static void ag_endpoint_root(const char *endpoint, char *out, size_t out_len)
{
   snprintf(out, out_len, "%s", endpoint ? endpoint : "");
   size_t len = strlen(out);
   if (len >= 3 && strcmp(out + len - 3, "/v1") == 0)
      out[len - 3] = '\0';
}

static void ag_join_url(const char *base, const char *suffix, char *out, size_t out_len)
{
   snprintf(out, out_len, "%s%s%s", base,
            (base && base[0] && base[strlen(base) - 1] == '/') ? "" : "/", suffix);
}

static int ag_json_int_field(cJSON *obj, const char *name)
{
   cJSON *v = cJSON_GetObjectItem(obj, name);
   return (v && cJSON_IsNumber(v)) ? v->valueint : 0;
}

static void ag_parse_slots_json(cJSON *root, int *slots_out, int *ctx_out)
{
   *slots_out = 0;
   *ctx_out = 0;

   if (cJSON_IsObject(root))
   {
      cJSON *arr = cJSON_GetObjectItem(root, "slots");
      if (arr && cJSON_IsArray(arr))
         root = arr;
      else
      {
         int slots = ag_json_int_field(root, "slots");
         if (!slots)
            slots = ag_json_int_field(root, "n_slots");
         if (!slots)
            slots = ag_json_int_field(root, "parallel");
         int ctx = ag_json_int_field(root, "n_ctx");
         if (!ctx)
            ctx = ag_json_int_field(root, "n_ctx_slot");
         if (!ctx)
            ctx = ag_json_int_field(root, "context_window");
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
      int ctx = ag_json_int_field(slot, "n_ctx");
      if (!ctx)
         ctx = ag_json_int_field(slot, "n_ctx_slot");
      if (!ctx)
         ctx = ag_json_int_field(slot, "context_window");
      if (ctx > 0 && (!min_ctx || ctx < min_ctx))
         min_ctx = ctx;
   }

   *slots_out = n;
   *ctx_out = min_ctx;
}

static int ag_probe_models(const char *endpoint, const char *requested_model, char *model_out,
                           size_t model_len, int *model_available, char *errbuf, size_t errbuf_len)
{
   model_out[0] = '\0';
   if (model_available)
      *model_available = 0;

   char url[MAX_ENDPOINT_LEN + 32];
   ag_join_url(endpoint, "models", url, sizeof(url));

   char *body = NULL;
   int status = agent_http_get(url, NULL, &body, 5000);
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
   if (!data || !cJSON_IsArray(data))
      data = cJSON_GetObjectItem(root, "models");

   if (data && cJSON_IsArray(data))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, data)
      {
         const char *id = NULL;
         if (cJSON_IsString(item))
            id = item->valuestring;
         else if (cJSON_IsObject(item))
            id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
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

static int ag_probe_slots(const char *endpoint, int *slots_out, int *ctx_out, char *errbuf,
                          size_t errbuf_len)
{
   *slots_out = 0;
   *ctx_out = 0;

   char root[MAX_ENDPOINT_LEN];
   char url[MAX_ENDPOINT_LEN + 32];
   ag_endpoint_root(endpoint, root, sizeof(root));
   ag_join_url(root, "slots", url, sizeof(url));

   char *body = NULL;
   int status = agent_http_get(url, NULL, &body, 5000);
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

   ag_parse_slots_json(json, slots_out, ctx_out);
   if (*slots_out <= 0)
      snprintf(errbuf, errbuf_len, "no slots found at %s", url);
   cJSON_Delete(json);
   return status;
}

static int ag_set_model_concurrency(const char *model, int limit)
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

static int ag_model_still_configured(const agent_config_t *cfg, const char *model)
{
   if (!cfg || !model || !model[0])
      return 0;
   for (int i = 0; i < cfg->agent_count; i++)
      if (strcmp(cfg->agents[i].model, model) == 0)
         return 1;
   return 0;
}

static int ag_clear_model_concurrency_if_unused(const agent_config_t *agents, const char *model)
{
   if (!model || !model[0] || ag_model_still_configured(agents, model))
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

static void ag_ensure_fallback(agent_config_t *cfg, const char *name)
{
   if (!name || !name[0])
      return;
   for (int i = 0; i < cfg->fallback_count; i++)
   {
      if (strcmp(cfg->fallback_chain[i], name) == 0)
         return;
   }
   if (cfg->fallback_count >= MAX_FALLBACK)
      cfg->fallback_count = MAX_FALLBACK - 1;
   memmove(&cfg->fallback_chain[1], &cfg->fallback_chain[0],
           (size_t)cfg->fallback_count * sizeof(cfg->fallback_chain[0]));
   snprintf(cfg->fallback_chain[0], sizeof(cfg->fallback_chain[0]), "%s", name);
   cfg->fallback_count++;
}

static void ag_remove_fallback(agent_config_t *cfg, const char *name)
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

/* --- agent subcommand handlers --- */

static void ag_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   agent_config_t *cfg = &s_agent_cfg;

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *ag = &cfg->agents[i];
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
         if (ag->exec_role_count > 0)
         {
            cJSON *er = cJSON_CreateArray();
            for (int j = 0; j < ag->exec_role_count; j++)
               cJSON_AddItemToArray(er, cJSON_CreateString(ag->exec_roles[j]));
            cJSON_AddItemToObject(obj, "exec_roles", er);
         }
         cJSON_AddItemToArray(arr, obj);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (cfg->agent_count == 0)
      {
         printf("No agents configured. Use 'aimee agent add' or "
                "'aimee agent local <name> <endpoint> --model MODEL' or edit %s\n",
                agent_config_path());
         return;
      }
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *ag = &cfg->agents[i];
         printf("%-16s %-6s tier=%d parallel=%d model=%s endpoint=%s%s\n", ag->name,
                ag->enabled ? "ON" : "OFF", ag->cost_tier, ag->max_parallel, ag->model,
                ag->endpoint, ag->tools_enabled ? " [tools]" : "");
      }
   }
}

static void ag_network(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   agent_network_t *nw = &s_agent_cfg.network;

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      if (nw->ssh_entry[0])
         cJSON_AddStringToObject(obj, "ssh_entry", nw->ssh_entry);
      if (nw->ssh_key[0])
         cJSON_AddStringToObject(obj, "ssh_key", nw->ssh_key);
      if (nw->host_count > 0)
      {
         cJSON *hosts = cJSON_CreateArray();
         for (int i = 0; i < nw->host_count; i++)
         {
            agent_net_host_t *h = &nw->hosts[i];
            cJSON *ho = cJSON_CreateObject();
            cJSON_AddStringToObject(ho, "name", h->name);
            cJSON_AddStringToObject(ho, "ip", h->ip);
            cJSON_AddStringToObject(ho, "user", h->user);
            if (h->port > 0)
               cJSON_AddNumberToObject(ho, "port", h->port);
            cJSON_AddStringToObject(ho, "desc", h->desc);
            cJSON_AddItemToArray(hosts, ho);
         }
         cJSON_AddItemToObject(obj, "hosts", hosts);
      }
      if (nw->network_count > 0)
      {
         cJSON *nets = cJSON_CreateArray();
         for (int i = 0; i < nw->network_count; i++)
         {
            agent_net_def_t *nd = &nw->networks[i];
            cJSON *no = cJSON_CreateObject();
            cJSON_AddStringToObject(no, "name", nd->name);
            cJSON_AddStringToObject(no, "cidr", nd->cidr);
            cJSON_AddStringToObject(no, "desc", nd->desc);
            cJSON_AddItemToArray(nets, no);
         }
         cJSON_AddItemToObject(obj, "networks", nets);
      }
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (!nw->ssh_entry[0])
      {
         printf("No network configured. Edit %s to add a \"network\" section.\n",
                agent_config_path());
         return;
      }
      printf("Entry point: %s\n", nw->ssh_entry);
      if (nw->ssh_key[0])
         printf("SSH key:     %s\n", nw->ssh_key);
      if (nw->host_count > 0)
      {
         printf("\nHosts:\n");
         for (int i = 0; i < nw->host_count; i++)
         {
            agent_net_host_t *h = &nw->hosts[i];
            if (h->port > 0)
               printf("  %-16s %s:%d  %-8s %s\n", h->name, h->ip, h->port, h->user, h->desc);
            else
               printf("  %-16s %-20s %-8s %s\n", h->name, h->ip, h->user, h->desc);
         }
      }
      if (nw->network_count > 0)
      {
         printf("\nNetworks:\n");
         for (int i = 0; i < nw->network_count; i++)
         {
            agent_net_def_t *nd = &nw->networks[i];
            printf("  %-16s %-20s %s\n", nd->name, nd->cidr, nd->desc);
         }
      }
   }
}

static void ag_test(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee agent test <name>");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);

   agent_http_init();
   agent_result_t result;
   int rc = agent_execute(ag, NULL, "Respond with 'ok'.", 64, 0.0, &result);
   agent_http_cleanup();

   if (rc == 0)
   {
      printf("Agent '%s' responded: %s\n", ag->name, result.response ? result.response : "(empty)");
      printf("Latency: %dms, Tokens: %d/%d\n", result.latency_ms, result.prompt_tokens,
             result.completion_tokens);
   }
   else
   {
      fprintf(stderr, "Agent '%s' failed: %s\n", ag->name, result.error);
   }
   free(result.response);
}

static void ag_run(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("usage: aimee agent run <role> \"prompt\" [--system S]");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *role = opt_pos(&opts, 0);
   const char *prompt = opt_pos(&opts, 1);
   const char *sys_prompt = opt_get(&opts, "system");
   int max_tokens = opt_get_int(&opts, "max-tokens", 0);

   if (!role || !prompt)
      fatal("usage: aimee agent run <role> \"prompt\" [--system S]");

   agent_http_init();
   agent_result_t result;
   int rc = agent_run(&s_agent_cfg, role, sys_prompt, prompt, max_tokens, &result);
   agent_http_cleanup();

   if (rc == 0)
   {
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "agent", result.agent_name);
         cJSON_AddStringToObject(obj, "response", result.response ? result.response : "");
         cJSON_AddNumberToObject(obj, "prompt_tokens", result.prompt_tokens);
         cJSON_AddNumberToObject(obj, "completion_tokens", result.completion_tokens);
         cJSON_AddNumberToObject(obj, "latency_ms", result.latency_ms);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("%s\n", result.response ? result.response : "");
      }
   }
   else
   {
      fatal("agent failed: %s", result.error);
   }
   free(result.response);
}

static void ag_parallel(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee agent parallel '<json tasks array>'\n"
            "  Each task: {\"role\":\"...\",\"prompt\":\"...\","
            "\"system\":\"...\",\"max_tokens\":N}");

   cJSON *tasks_json = cJSON_Parse(argv[0]);
   if (!tasks_json || !cJSON_IsArray(tasks_json))
      fatal("invalid JSON tasks array");

   int n = cJSON_GetArraySize(tasks_json);
   agent_task_t *tasks = calloc((size_t)n, sizeof(agent_task_t));
   agent_result_t *results = calloc((size_t)n, sizeof(agent_result_t));
   if (!tasks || !results)
   {
      free(tasks);
      free(results);
      cJSON_Delete(tasks_json);
      fatal("memory allocation failed");
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_GetArrayItem(tasks_json, i);
      cJSON *role = cJSON_GetObjectItem(t, "role");
      cJSON *prompt = cJSON_GetObjectItem(t, "prompt");
      cJSON *sys = cJSON_GetObjectItem(t, "system");
      cJSON *mt = cJSON_GetObjectItem(t, "max_tokens");
      tasks[i].role = (role && cJSON_IsString(role)) ? role->valuestring : "draft";
      tasks[i].user_prompt = (prompt && cJSON_IsString(prompt)) ? prompt->valuestring : "";
      tasks[i].system_prompt = (sys && cJSON_IsString(sys)) ? sys->valuestring : NULL;
      tasks[i].max_tokens = (mt && cJSON_IsNumber(mt)) ? mt->valueint : 0;
      tasks[i].temperature = 0.3;
   }

   agent_http_init();
   int ok = agent_run_parallel(&s_agent_cfg, tasks, n, results);
   agent_http_cleanup();

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "agent", results[i].agent_name);
      cJSON_AddBoolToObject(obj, "success", results[i].success);
      if (results[i].response)
         cJSON_AddStringToObject(obj, "response", results[i].response);
      if (results[i].error[0])
         cJSON_AddStringToObject(obj, "error", results[i].error);
      cJSON_AddNumberToObject(obj, "latency_ms", results[i].latency_ms);
      cJSON_AddItemToArray(arr, obj);
      free(results[i].response);
   }

   if (ctx->json_output)
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   else
   {
      printf("%d/%d tasks completed\n", ok, n);
      cJSON_Delete(arr);
   }

   free(tasks);
   free(results);
   cJSON_Delete(tasks_json);
}

static void ag_stats(app_ctx_t *ctx, int argc, char **argv)
{
   config_t db1_cfg;
   config_load(&db1_cfg);
   if (db1_init(db1_cfg.db1_path) != 0)
      fatal("agent stats: could not initialize DB1");
   const char *name = (argc >= 1) ? argv[0] : NULL;
   agent_stats_t stats[MAX_AGENTS];
   int n = agent_get_stats(name, stats, MAX_AGENTS);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", stats[i].name);
         cJSON_AddNumberToObject(obj, "total_calls", stats[i].total_calls);
         cJSON_AddNumberToObject(obj, "prompt_tokens", stats[i].total_prompt_tokens);
         cJSON_AddNumberToObject(obj, "completion_tokens", stats[i].total_completion_tokens);
         cJSON_AddNumberToObject(obj, "cache_write_tokens", stats[i].total_cache_write_tokens);
         cJSON_AddNumberToObject(obj, "cache_read_tokens", stats[i].total_cache_read_tokens);
         cJSON_AddNumberToObject(obj, "estimated_cost_usd", stats[i].total_estimated_cost_usd);
         cJSON_AddNumberToObject(obj, "avg_latency_ms", stats[i].avg_latency_ms);
         cJSON_AddNumberToObject(obj, "success_rate", stats[i].success_rate);
         cJSON_AddItemToArray(arr, obj);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (n == 0)
      {
         printf("No agent calls recorded.\n");
      }
      else
      {
         for (int i = 0; i < n; i++)
         {
            if (stats[i].total_estimated_cost_usd > 0.0)
               printf("%-16s calls=%d tokens=%d/%d cost=$%.4f avg=%dms "
                      "success=%.0f%%\n",
                      stats[i].name, stats[i].total_calls, stats[i].total_prompt_tokens,
                      stats[i].total_completion_tokens, stats[i].total_estimated_cost_usd,
                      stats[i].avg_latency_ms, stats[i].success_rate * 100);
            else
               printf("%-16s calls=%d tokens=%d/%d avg=%dms "
                      "success=%.0f%%\n",
                      stats[i].name, stats[i].total_calls, stats[i].total_prompt_tokens,
                      stats[i].total_completion_tokens, stats[i].avg_latency_ms,
                      stats[i].success_rate * 100);
         }
      }
   }
}

static void ag_add(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   agent_config_t *cfg = &s_agent_cfg;

   if (argc < 3)
      fatal("usage: aimee agent add <name> <endpoint> <model> "
            "[--key KEY] [--auth-cmd CMD] [--auth-type TYPE] "
            "[--provider openai|chatgpt] [--roles r1,r2,...] [--cost-tier N] "
            "[--tools|--tools-enabled] [--max-turns N] [--max-parallel N] "
            "[--ctx N] [--timeout-ms N] [--exec-roles r1,r2,...]\n"
            "  --max-turns: per-agent delegate turn cap; 0 = unlimited "
            "(frontier agents), omitted = inherit the role floor.");

   char old_model[MAX_MODEL_LEN] = {0};
   int was_empty = cfg->agent_count == 0;
   agent_t *ag = agent_find(cfg, argv[0]);
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count++];
   }
   else
      snprintf(old_model, sizeof(old_model), "%s", ag->model);

   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, MAX_AGENT_NAME, "%s", argv[0]);
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", argv[1]);
   snprintf(ag->model, MAX_MODEL_LEN, "%s", argv[2]);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = AGENT_DEFAULT_TIMEOUT_MS;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   ag->enabled = 1;

   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
      {
         /* Preserve the verbatim reference (e.g. "$VAR") for save so a resolved
          * secret is never written to agents.json; resolve a runtime copy.
          * Mirrors agent_load_config. */
         const char *raw_key = argv[++i];
         snprintf(ag->api_key_disk, MAX_API_KEY_LEN, "%s", raw_key);
         agent_expand_env(raw_key, ag->api_key, MAX_API_KEY_LEN);
      }
      else if (strcmp(argv[i], "--auth-cmd") == 0 && i + 1 < argc)
         snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", argv[++i]);
      else if (strcmp(argv[i], "--auth-type") == 0 && i + 1 < argc)
         snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", argv[++i]);
      else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
         snprintf(ag->provider, sizeof(ag->provider), "%s", argv[++i]);
      else if (strcmp(argv[i], "--roles") == 0 && i + 1 < argc)
         ag_set_roles_csv(ag, argv[++i]);
      else if (strcmp(argv[i], "--cost-tier") == 0 && i + 1 < argc)
         ag->cost_tier = atoi(argv[++i]);
      else if (strcmp(argv[i], "--tools-enabled") == 0 || strcmp(argv[i], "--tools") == 0)
         ag->tools_enabled = 1;
      else if (strcmp(argv[i], "--max-turns") == 0 && i + 1 < argc)
         ag->max_turns = atoi(argv[++i]);
      else if (strcmp(argv[i], "--max-parallel") == 0 && i + 1 < argc)
         ag->max_parallel = atoi(argv[++i]);
      else if ((strcmp(argv[i], "--ctx") == 0 || strcmp(argv[i], "--context-window") == 0) &&
               i + 1 < argc)
         ag->middleware.context_window = atoi(argv[++i]);
      else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc)
         ag->max_tokens = atoi(argv[++i]);
      else if ((strcmp(argv[i], "--timeout-ms") == 0 || strcmp(argv[i], "--timeout") == 0) &&
               i + 1 < argc)
         ag->timeout_ms = atoi(argv[++i]);
      else if (strcmp(argv[i], "--exec-roles") == 0 && i + 1 < argc)
         ag_set_exec_roles_csv(ag, argv[++i]);
   }

   /* Default roles if none specified */
   if (ag->role_count == 0)
   {
      snprintf(ag->roles[0], sizeof(ag->roles[0]), "summarize");
      snprintf(ag->roles[1], sizeof(ag->roles[1]), "format");
      snprintf(ag->roles[2], sizeof(ag->roles[2]), "draft");
      ag->role_count = 3;
   }

   /* Set as default if first agent */
   if (was_empty)
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", ag->name);

   agent_save_config(cfg);
   if (ag->max_parallel > 0 && ag->max_parallel != AGENT_DEFAULT_MAX_PARALLEL)
      (void)ag_set_model_concurrency(ag->model, ag->max_parallel);
   if (old_model[0] && strcmp(old_model, ag->model) != 0)
      (void)ag_clear_model_concurrency_if_unused(cfg, old_model);
   printf("Agent '%s' added.\n", ag->name);
}

static void ag_remove(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   agent_config_t *cfg = &s_agent_cfg;

   if (argc < 1)
      fatal("usage: aimee agent remove <name>");
   int found = -1;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, argv[0]) == 0)
      {
         found = i;
         break;
      }
   }
   if (found < 0)
      fatal("agent '%s' not found", argv[0]);
   char removed_model[MAX_MODEL_LEN];
   snprintf(removed_model, sizeof(removed_model), "%s", cfg->agents[found].model);
   memmove(&cfg->agents[found], &cfg->agents[found + 1],
           (size_t)(cfg->agent_count - found - 1) * sizeof(agent_t));
   cfg->agent_count--;
   ag_remove_fallback(cfg, argv[0]);
   if (strcmp(cfg->default_agent, argv[0]) == 0)
      snprintf(cfg->default_agent, sizeof(cfg->default_agent), "%s",
               cfg->agent_count > 0 ? cfg->agents[0].name : "");
   agent_save_config(cfg);
   (void)ag_clear_model_concurrency_if_unused(cfg, removed_model);
   printf("Agent '%s' removed.\n", argv[0]);
}

static void ag_enable(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
      fatal("usage: aimee agent enable <name>");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);
   ag->enabled = 1;
   agent_save_config(&s_agent_cfg);
   printf("Agent '%s' enabled.\n", argv[0]);
}

static void ag_disable(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
      fatal("usage: aimee agent disable <name>");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);
   ag->enabled = 0;
   agent_save_config(&s_agent_cfg);
   printf("Agent '%s' disabled.\n", argv[0]);
}

static void ag_local(app_ctx_t *ctx, int argc, char **argv)
{
   const char *bool_flags[] = {
       "default", "no-probe", "no-tools", "no-fallback", "recommended-sampling", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *name = opt_get(&opts, "name");
   const char *endpoint_arg = opt_get(&opts, "endpoint");
   const char *model_arg = opt_get(&opts, "model");
   const char *provider_arg = opt_get(&opts, "provider");
   const char *roles_arg = opt_get(&opts, "roles");
   const char *exec_roles_arg = opt_get(&opts, "exec-roles");

   const char *p0 = opt_pos(&opts, 0);
   const char *p1 = opt_pos(&opts, 1);
   if (!endpoint_arg && p0 && ag_looks_like_endpoint(p0))
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
      fatal("usage: aimee agent local [name] <endpoint> --model MODEL "
            "[--slots N] [--ctx N] [--provider openai|ollama|llama_native|llama-eval] "
            "[--default] [--no-probe]");

   char endpoint[MAX_ENDPOINT_LEN];
   ag_normalize_endpoint(endpoint_arg, endpoint, sizeof(endpoint));

   char model[MAX_MODEL_LEN] = {0};
   if (model_arg && model_arg[0])
      snprintf(model, sizeof(model), "%s", model_arg);

   int slots = opt_get_int(&opts, "slots", 0);
   int context_window = opt_get_int(&opts, "ctx", 0);
   if (!context_window)
      context_window = opt_get_int(&opts, "context-window", 0);

   int detected_slots = 0;
   int detected_ctx = 0;
   int model_available = model[0] ? 0 : 1;
   char model_probe_msg[256] = {0};
   char slot_probe_msg[256] = {0};
   hardware_probe_result_t hw;
   hardware_probe_result_init(&hw);

   if (!opt_has(&opts, "no-probe"))
   {
      agent_http_init();

      char detected_model[MAX_MODEL_LEN] = {0};
      (void)ag_probe_models(endpoint, model[0] ? model : NULL, detected_model,
                            sizeof(detected_model), &model_available, model_probe_msg,
                            sizeof(model_probe_msg));
      if (!model[0] && detected_model[0])
         snprintf(model, sizeof(model), "%s", detected_model);

      (void)ag_probe_slots(endpoint, &detected_slots, &detected_ctx, slot_probe_msg,
                           sizeof(slot_probe_msg));
      agent_http_cleanup();
   }

   if (!model[0])
      fatal("could not determine model for %s; pass --model MODEL", endpoint);
   if (slots <= 0 && detected_slots > 0)
      slots = detected_slots;
   if (slots <= 0)
      slots = 1;
   if (!opt_has(&opts, "no-probe"))
      (void)hardware_probe_cached_or_detect(&hw);
   if (context_window <= 0)
   {
      int derived_ctx =
          hardware_probe_context_window_from_vram(hw.detected ? hw.vram_mb : 0, detected_ctx);
      if (derived_ctx > 0)
         context_window = derived_ctx;
      else if (detected_ctx > 0)
         context_window = detected_ctx;
   }

   agent_config_t *cfg = &s_agent_cfg;
   char old_model[MAX_MODEL_LEN] = {0};
   agent_t *ag = agent_find(cfg, name);
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count++];
   }
   else
      snprintf(old_model, sizeof(old_model), "%s", ag->model);
   memset(ag, 0, sizeof(*ag));

   snprintf(ag->name, sizeof(ag->name), "%s", name);
   snprintf(ag->endpoint, sizeof(ag->endpoint), "%s", endpoint);
   snprintf(ag->model, sizeof(ag->model), "%s", model);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "none");
   snprintf(ag->provider, sizeof(ag->provider), "%s", ag_canonical_local_provider(provider_arg));
   ag->cost_tier = opt_get_int(&opts, "cost-tier", 0);
   ag->max_tokens = opt_get_int(&opts, "max-tokens", AGENT_DEFAULT_MAX_TOKENS);
   ag->timeout_ms = opt_get_int(&opts, "timeout-ms", opt_get_int(&opts, "timeout", 300000));
   ag->enabled = 1;
   ag->tools_enabled = opt_has(&opts, "no-tools") ? 0 : 1;
   ag->recommended_sampling = opt_has(&opts, "recommended-sampling") ? 1 : 0;
   ag->inject_respond_tool = ag->tools_enabled ? 1 : 0;
   ag->max_turns = opt_get_int(&opts, "max-turns", -1);
   ag->max_parallel = slots;
   ag->middleware.context_window = context_window;

   if (roles_arg && roles_arg[0])
      ag_set_roles_csv(ag, roles_arg);
   else
      ag_set_default_delegate_roles(ag);
   if (exec_roles_arg && exec_roles_arg[0])
      ag_set_exec_roles_csv(ag, exec_roles_arg);

   if (opt_has(&opts, "default") || cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, sizeof(cfg->default_agent), "%s", ag->name);
   if (!opt_has(&opts, "no-fallback"))
      ag_ensure_fallback(cfg, ag->name);

   if (agent_save_config(cfg) != 0)
      fatal("could not save %s", agent_config_path());
   if (ag_set_model_concurrency(ag->model, ag->max_parallel) != 0)
      fatal("could not update delegate concurrency in %s", config_default_path());
   if (old_model[0] && strcmp(old_model, ag->model) != 0)
      (void)ag_clear_model_concurrency_if_unused(cfg, old_model);

   int estimated_vram_mb = 0;
   if (hardware_probe_should_warn_fit(&hw, ag->model, ag->middleware.context_window,
                                      &estimated_vram_mb))
   {
      fprintf(stderr, "warning: %s may exceed detected %d MB %s VRAM (estimated footprint %d MB)\n",
              ag->model, hw.vram_mb, hw.vendor[0] ? hw.vendor : "GPU", estimated_vram_mb);
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "name", ag->name);
      cJSON_AddStringToObject(obj, "endpoint", ag->endpoint);
      cJSON_AddStringToObject(obj, "model", ag->model);
      cJSON_AddNumberToObject(obj, "max_parallel", ag->max_parallel);
      if (ag->middleware.context_window > 0)
         cJSON_AddNumberToObject(obj, "context_window", ag->middleware.context_window);
      cJSON_AddBoolToObject(obj, "tools_enabled", ag->tools_enabled);
      cJSON_AddBoolToObject(obj, "recommended_sampling", ag->recommended_sampling);
      cJSON_AddBoolToObject(obj, "inject_respond_tool", ag->inject_respond_tool);
      cJSON_AddBoolToObject(obj, "model_available", model_available ? 1 : 0);
      if (model_probe_msg[0])
         cJSON_AddStringToObject(obj, "model_probe", model_probe_msg);
      if (slot_probe_msg[0])
         cJSON_AddStringToObject(obj, "slot_probe", slot_probe_msg);
      if (hw.detected)
      {
         cJSON_AddNumberToObject(obj, "gpu_vram_mb", hw.vram_mb);
         cJSON_AddStringToObject(obj, "gpu_name", hw.name);
         cJSON_AddStringToObject(obj, "gpu_vendor", hw.vendor);
      }
      if (estimated_vram_mb > 0)
         cJSON_AddNumberToObject(obj, "estimated_model_vram_mb", estimated_vram_mb);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Local delegate '%s' registered: %s @ %s\n", ag->name, ag->model, ag->endpoint);
   printf("  max_parallel=%d", ag->max_parallel);
   if (ag->middleware.context_window > 0)
      printf(" context_window=%d", ag->middleware.context_window);
   printf(" tools=%s sampling=%s\n", ag->tools_enabled ? "on" : "off",
          ag->recommended_sampling ? "recommended" : "default");
   if (model_probe_msg[0] && !model_available)
      printf("  warning: %s\n", model_probe_msg);
   if (slot_probe_msg[0] && detected_slots <= 0 && !opt_has(&opts, "no-probe"))
      printf("  warning: %s\n", slot_probe_msg);
}

static void ag_probe(app_ctx_t *ctx, int argc, char **argv)
{
   const char *bool_flags[] = {"no-run", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *name = opt_pos(&opts, 0);
   if (!name || !name[0])
      fatal("usage: aimee agent probe <name> [--no-run]");
   agent_t *ag = agent_find(&s_agent_cfg, name);
   if (!ag)
      fatal("agent '%s' not found", name);

   char model_found[MAX_MODEL_LEN] = {0};
   char model_msg[256] = {0};
   char slots_msg[256] = {0};
   int model_available = 0;
   int slots = 0;
   int context_window = 0;
   int run_ok = 0;
   int latency_ms = 0;
   char run_msg[512] = {0};

   agent_http_init();
   int models_status = ag_probe_models(ag->endpoint, ag->model, model_found, sizeof(model_found),
                                       &model_available, model_msg, sizeof(model_msg));
   int slots_status =
       ag_probe_slots(ag->endpoint, &slots, &context_window, slots_msg, sizeof(slots_msg));

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
   agent_http_cleanup();

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "name", ag->name);
      cJSON_AddStringToObject(obj, "endpoint", ag->endpoint);
      cJSON_AddStringToObject(obj, "model", ag->model);
      cJSON_AddNumberToObject(obj, "models_status", models_status);
      cJSON_AddBoolToObject(obj, "model_available", model_available ? 1 : 0);
      cJSON_AddNumberToObject(obj, "slots_status", slots_status);
      cJSON_AddNumberToObject(obj, "detected_slots", slots);
      if (context_window > 0)
         cJSON_AddNumberToObject(obj, "detected_context_window", context_window);
      if (!opt_has(&opts, "no-run"))
      {
         cJSON_AddBoolToObject(obj, "execution_ok", run_ok ? 1 : 0);
         cJSON_AddNumberToObject(obj, "latency_ms", latency_ms);
         cJSON_AddStringToObject(obj, "execution_message", run_msg);
      }
      if (model_msg[0])
         cJSON_AddStringToObject(obj, "model_probe", model_msg);
      if (slots_msg[0])
         cJSON_AddStringToObject(obj, "slot_probe", slots_msg);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("%s\n", ag->name);
      printf("  models: %s (%d)\n", model_available ? "ok" : "warn", models_status);
      if (model_msg[0])
         printf("  model_probe: %s\n", model_msg);
      printf("  slots: %d", slots);
      if (context_window > 0)
         printf(" context_window=%d", context_window);
      printf(" (%d)\n", slots_status);
      if (slots_msg[0])
         printf("  slot_probe: %s\n", slots_msg);
      if (!opt_has(&opts, "no-run"))
         printf("  execution: %s latency=%dms %s\n", run_ok ? "ok" : "failed", latency_ms, run_msg);
   }

   if (!model_available || (!opt_has(&opts, "no-run") && !run_ok))
      exit(1);
}

/* --- key file helper --- */

/* Forward declarations for functions in cmd_agent_setup.c */
void ag_setup(app_ctx_t *ctx, int argc, char **argv);
void ag_token(app_ctx_t *ctx, int argc, char **argv);
void ag_tunnel(app_ctx_t *ctx, int argc, char **argv);

/* --- agent subcommand table --- */

static const subcmd_t agent_subcmds[] = {
    {"list", "List configured agents", ag_list},
    {"network", "Show network/host configuration", ag_network},
    {"tunnel", "Show tunnel configuration", ag_tunnel},
    {"test", "Test connectivity to an agent", ag_test},
    {"run", "Run a prompt on a specific agent", ag_run},
    {"parallel", "Run a prompt across multiple agents", ag_parallel},
    {"stats", "Show agent usage statistics", ag_stats},
    {"add", "Add a new agent", ag_add},
    {"local", "Register or update a local OpenAI-compatible delegate", ag_local},
    {"probe", "Probe delegate endpoint, slots, and execution", ag_probe},
    {"remove", "Remove an agent", ag_remove},
    {"enable", "Enable a disabled agent", ag_enable},
    {"disable", "Disable an agent", ag_disable},
    {"setup", "Interactive agent setup wizard", ag_setup},
    {"token", "Refresh or show agent auth token", ag_token},
    {NULL, NULL, NULL},
};

const subcmd_t *get_agent_subcmds(void)
{
   return agent_subcmds;
}

/* --- cmd_agent --- */

void cmd_agent(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("agent", agent_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (agent_load_config(&s_agent_cfg) != 0)
      memset(&s_agent_cfg, 0, sizeof(s_agent_cfg));

   if (subcmd_dispatch(agent_subcmds, sub, ctx, argc, argv) != 0)
      fatal("unknown agent subcommand: %s", sub);
}

/* Plans and eval CLI commands. */

/* --- cmd_plans --- */

void cmd_plans(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Plan IR CLI (Feature 2) */
   if (argc < 1)
      fatal("usage: aimee plans list|show|verify <id>");

   if (strcmp(argv[0], "list") == 0)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("plans list: could not initialize DB1");
      plan_t plans[20];
      int count = db1_execution_plan_list(plans, 20);
      printf("%-6s %-12s %-10s %s\n", "ID", "Agent", "Status", "Task");
      for (int i = 0; i < count; i++)
      {
         printf("%-6d %-12s %-10s %.*s\n", plans[i].id, plans[i].agent_name, plans[i].status, 60,
                plans[i].task);
      }
   }
   else if (strcmp(argv[0], "show") == 0 && argc >= 2)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("plans show: could not initialize DB1");
      int pid = atoi(argv[1]);
      plan_t plan;
      if (db1_execution_plan_get(pid, &plan) != 0)
         fatal("plan %d not found", pid);

      printf("Plan #%d [%s]\n", plan.id, plan.status);
      printf("Agent: %s\n", plan.agent_name);
      printf("Task:  %s\n\n", plan.task);
      printf("Steps:\n");
      static const char *status_names[] = {"pending", "running", "done", "failed", "rolled_back"};
      for (int i = 0; i < plan.step_count; i++)
      {
         plan_step_t *s = &plan.steps[i];
         const char *sn = (s->status >= 0 && s->status <= 4) ? status_names[s->status] : "unknown";
         printf("  %d. [%s] %s\n", i + 1, sn, s->action);
         if (s->precondition[0])
            printf("     Precondition: %s\n", s->precondition);
         if (s->success_predicate[0])
            printf("     Success: %s\n", s->success_predicate);
         if (s->rollback[0])
            printf("     Rollback: %s\n", s->rollback);
         if (s->output[0])
            printf("     Output: %.*s\n", 200, s->output);

         db1_step_evidence_latest_t evidence;
         if (db1_step_evidence_get_latest(s->id, &evidence) == 0)
         {
            printf("     Evidence: %s %s via %s (%s)\n",
                   evidence.strength[0] ? evidence.strength : "weak",
                   evidence.passed ? "pass" : "fail", evidence.kind[0] ? evidence.kind : "unknown",
                   evidence.created_at);
         }
      }
   }
   else if (strcmp(argv[0], "verify") == 0 && argc >= 2)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("plans verify: could not initialize DB1");
      int pid = atoi(argv[1]);
      plan_t plan;
      plan_verify_summary_t summary;
      if (db1_execution_plan_get(pid, &plan) != 0)
         fatal("plan %d not found", pid);

      int rc = agent_plan_verify(&plan, &summary);
      printf("Plan #%d verification: %s\n", plan.id, rc == 0 ? "passed" : "failed");
      printf("  Strong evidence: %d\n", summary.strong_passed);
      printf("  Weak evidence:   %d\n", summary.weak_passed);
      printf("  Failed steps:    %d\n", summary.failed);
   }
   else if (strcmp(argv[0], "replay") == 0 && argc >= 2)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("plans replay: could not initialize DB1");
      int pid = atoi(argv[1]);
      plan_t plan;
      if (db1_execution_plan_get(pid, &plan) != 0)
         fatal("plan %d not found", pid);

      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0)
         fatal("no agents configured");

      agent_t *ag = agent_find(&acfg, plan.agent_name);
      int timeout = ag ? ag->timeout_ms : AGENT_DEFAULT_TIMEOUT_MS;
      int rc = agent_plan_execute(&plan, ag, timeout);
      printf("Replay %s.\n", rc == 0 ? "succeeded" : "failed");
   }
}

/* --- cmd_eval --- */

static void print_mem_eval_report(const char *title, const mem_eval_scores_t *scores,
                                  const mem_eval_latency_t *latency)
{
   printf("%s (%d cases)\n", title, scores->n_cases);
   printf("  MRR:       %.4f\n", scores->mrr);
   printf("  NDCG@5:    %.4f\n", scores->ndcg_5);
   printf("  NDCG@10:   %.4f\n", scores->ndcg_10);
   printf("  Recall@5:  %.4f\n", scores->recall_5);
   printf("  Recall@10: %.4f\n", scores->recall_10);
   if (latency && latency->n_queries > 0)
   {
      printf("  Latency:   p50=%.3fms p95=%.3fms p99=%.3fms min=%.3fms max=%.3fms (%d queries)\n",
             latency->p50_ms, latency->p95_ms, latency->p99_ms, latency->min_ms, latency->max_ms,
             latency->n_queries);
   }
}

static void print_mem_eval_qa_report(const char *title, const mem_eval_qa_scores_t *scores,
                                     const mem_eval_latency_t *latency)
{
   printf("%s (%d cases)\n", title, scores->n_cases);
   printf("  Accuracy:              %.4f\n", scores->accuracy);
   printf("  Hallucination Rate:    %.4f\n", scores->hallucination_rate);
   printf("  Exact Match:           %.4f\n", scores->exact_match);
   printf("  Citation Coverage:     %.4f\n", scores->citation_coverage);
   printf("  Citation Miss Rate:    %.4f\n", scores->citation_miss_rate);
   printf("  Answered Cases:        %d\n", scores->answered_cases);
   printf("  Judged Cases:          %d\n", scores->judged_cases);
   printf("  Cited Answers:         %d\n", scores->cited_answers);
   printf("  Uncited Answers:       %d\n", scores->uncited_answers);
   printf("  Low-Confidence Answers:%d\n", scores->low_confidence_answers);
   printf("  Avg Retrieved Tokens:  %.1f\n", scores->avg_retrieved_tokens);
   printf("  Answer Tokens:         prompt=%d completion=%d\n", scores->total_answer_prompt_tokens,
          scores->total_answer_completion_tokens);
   printf("  Judge Tokens:          prompt=%d completion=%d\n", scores->total_judge_prompt_tokens,
          scores->total_judge_completion_tokens);
   if (latency && latency->n_queries > 0)
   {
      printf("  End-to-End Latency:    p50=%.3fms p95=%.3fms p99=%.3fms min=%.3fms max=%.3fms (%d "
             "cases)\n",
             latency->p50_ms, latency->p95_ms, latency->p99_ms, latency->min_ms, latency->max_ms,
             latency->n_queries);
   }
}

static int load_live_mem_eval_cases(mem_eval_case_t *cases, int max_cases, char *basis,
                                    size_t basis_len)
{
   if (max_cases <= 0)
   {
      if (basis && basis_len > 0)
         basis[0] = '\0';
      return 0;
   }

   memory_t rows[100];
   int row_cap = max_cases < (int)(sizeof(rows) / sizeof(rows[0]))
                     ? max_cases
                     : (int)(sizeof(rows) / sizeof(rows[0]));
   int n_rows = kb_client_memory_load_eval_corpus(rows, row_cap, basis, basis_len);
   if (n_rows <= 0)
      return 0;

   memset(cases, 0, (size_t)max_cases * sizeof(*cases));
   int n_cases = 0;
   for (int i = 0; i < n_rows && n_cases < max_cases; i++)
   {
      const char *key = rows[i].key;
      const char *content = rows[i].content;
      snprintf(cases[n_cases].query, sizeof(cases[n_cases].query), "%s",
               (key && key[0]) ? key : (content ? content : ""));
      if (!cases[n_cases].query[0])
         continue;
      cases[n_cases].expected_ids[0] = rows[i].id;
      cases[n_cases].n_expected = 1;
      n_cases++;
   }
   return n_cases;
}

void cmd_eval(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Eval Harness CLI (Feature 6) */
   if (argc < 1)
      fatal(
          "usage: aimee eval run <suite_dir>|results [suite]|memory-retrieval|locomo|longmemeval");

   if (strcmp(argv[0], "memory-retrieval") == 0)
   {
      /* Parse optional flags: --corpus <path>, --baseline <path>, --update-baseline */
      const char *corpus_path = "tests/eval/memory_retrieval_corpus.json";
      const char *baseline_path = "tests/eval/memory_retrieval_baseline.json";
      int update_baseline = 0;
      int corpus_explicit = 0;

      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
         {
            corpus_path = argv[++i];
            corpus_explicit = 1;
         }
         else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc)
         {
            baseline_path = argv[++i];
         }
         else if (strcmp(argv[i], "--update-baseline") == 0)
         {
            update_baseline = 1;
         }
      }

      /* Try corpus-based eval first. */
      static mem_eval_case_t corpus_cases[MEM_CORPUS_MAX_CASES];
      int n_corpus = mem_eval_load_corpus(corpus_path, corpus_cases, MEM_CORPUS_MAX_CASES);

      if (n_corpus > 0)
      {
         mem_eval_scores_t scores;
         mem_eval_latency_t latency;
         mem_eval_run_with_latency(corpus_cases, n_corpus, &scores, &latency);

         mem_eval_close_temp_db();

         char title[1024];
         snprintf(title, sizeof(title), "Memory Retrieval Evaluation — corpus: %s", corpus_path);
         print_mem_eval_report(title, &scores, &latency);

         if (update_baseline)
         {
            if (mem_eval_save_baseline(baseline_path, &scores, 5.0) == 0)
               printf("Baseline updated: %s\n", baseline_path);
            else
               fprintf(stderr, "error: could not write baseline to %s\n", baseline_path);
            return;
         }

         /* Regression check against baseline. */
         mem_eval_scores_t baseline;
         double threshold_pct = 5.0;
         if (mem_eval_load_baseline(baseline_path, &baseline, &threshold_pct) == 0)
         {
            if (mem_eval_check_regression(&scores, &baseline, threshold_pct) != 0)
            {
               fprintf(stderr, "FAIL: memory retrieval regression detected vs %s\n", baseline_path);
               exit(1);
            }
            printf("OK: no regression vs baseline (%s, threshold %.1f%%)\n", baseline_path,
                   threshold_pct);
         }
         else
         {
            printf("note: no baseline found at %s (run with --update-baseline to create one)\n",
                   baseline_path);
         }

         return;
      }

      if (corpus_explicit || access(corpus_path, R_OK) == 0)
      {
         fprintf(stderr, "error: could not load memory retrieval corpus %s\n", corpus_path);
         exit(1);
      }

      /* Corpus not found in an installed/non-repo environment: fall back to live-DB mode. */
      if (n_corpus < 0)
         fprintf(stderr, "note: corpus not available at %s, using live database\n", corpus_path);

      mem_eval_case_t cases[100];
      char basis[64];
      int n_cases = load_live_mem_eval_cases(cases, 100, basis, sizeof(basis));

      if (n_cases == 0)
      {
         printf("No suitable memories in database. Add memories first.\n");
         return;
      }

      mem_eval_scores_t scores;
      mem_eval_latency_t latency;
      mem_eval_run_with_latency(cases, n_cases, &scores, &latency);

      char title[128];
      snprintf(title, sizeof(title), "Memory Retrieval Evaluation — live DB (%s)",
               basis[0] ? basis : "seeded");
      print_mem_eval_report(title, &scores, &latency);

      return;
   }
   else if (strcmp(argv[0], "locomo") == 0)
   {
      const char *dataset_path = "data/locomo/locomo10.json";
      int report_misses = 0;
      int misses_only = 0;
      int max_misses = 20;
      int miss_limit = 5;
      int max_samples = 0; /* 0 = no cap */
      const char *progress_path = NULL;
      const char *miss_progress_path = NULL;
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc)
            dataset_path = argv[++i];
         else if (strcmp(argv[i], "--progress-jsonl") == 0 && i + 1 < argc)
            progress_path = argv[++i];
         else if (strcmp(argv[i], "--miss-progress-jsonl") == 0 && i + 1 < argc)
            miss_progress_path = argv[++i];
         else if (strcmp(argv[i], "--report-misses") == 0)
            report_misses = 1;
         else if (strcmp(argv[i], "--misses-only") == 0)
            misses_only = 1;
         else if (strcmp(argv[i], "--max-misses") == 0 && i + 1 < argc)
            max_misses = atoi(argv[++i]);
         else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            miss_limit = atoi(argv[++i]);
         else if (strcmp(argv[i], "--max-samples") == 0 && i + 1 < argc)
            max_samples = atoi(argv[++i]);
      }

      if (!misses_only)
      {
         mem_eval_scores_t scores;
         mem_eval_scores_t support_scores;
         mem_eval_latency_t latency;
         mem_eval_latency_t support_latency;
         int samples = 0;
         int support_samples = 0;
         if (mem_eval_run_locomo(dataset_path, max_samples, &scores, &latency, &samples,
                                 progress_path) != 0)
            fatal("LoCoMo eval failed for %s", dataset_path);
         if (mem_eval_run_locomo_session_support(dataset_path, max_samples, &support_scores,
                                                 &support_latency, &support_samples) != 0)
         {
            memset(&support_scores, 0, sizeof(support_scores));
            memset(&support_latency, 0, sizeof(support_latency));
         }

         char title[1024];
         snprintf(title, sizeof(title), "LoCoMo Retrieval Evaluation — %s (%d conversations)",
                  dataset_path, samples);
         print_mem_eval_report(title, &scores, &latency);
         if (support_scores.n_cases > 0)
         {
            snprintf(title, sizeof(title),
                     "LoCoMo Session-Support Retrieval — %s (%d conversations)", dataset_path,
                     support_samples);
            print_mem_eval_report(title, &support_scores, &support_latency);
         }
      }
      if (report_misses)
         (void)mem_eval_report_locomo_misses(dataset_path, max_samples, miss_limit, max_misses,
                                             stdout, miss_progress_path);
      return;
   }
   else if (strcmp(argv[0], "longmemeval") == 0)
   {
      const char *dataset_path = "data/longmemeval/longmemeval_s_cleaned.json";
      int report_misses = 0;
      int misses_only = 0;
      int max_misses = 20;
      int miss_limit = 5;
      int max_cases = 0;
      const char *progress_path = NULL;
      const char *miss_progress_path = NULL;
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc)
            dataset_path = argv[++i];
         else if (strcmp(argv[i], "--progress-jsonl") == 0 && i + 1 < argc)
            progress_path = argv[++i];
         else if (strcmp(argv[i], "--miss-progress-jsonl") == 0 && i + 1 < argc)
            miss_progress_path = argv[++i];
         else if (strcmp(argv[i], "--report-misses") == 0)
            report_misses = 1;
         else if (strcmp(argv[i], "--misses-only") == 0)
            misses_only = 1;
         else if (strcmp(argv[i], "--max-misses") == 0 && i + 1 < argc)
            max_misses = atoi(argv[++i]);
         else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            miss_limit = atoi(argv[++i]);
         else if (strcmp(argv[i], "--max-cases") == 0 && i + 1 < argc)
            max_cases = atoi(argv[++i]);
      }

      if (!misses_only)
      {
         mem_eval_scores_t scores;
         mem_eval_latency_t latency;
         int cases = 0;
         if (mem_eval_run_longmemeval(dataset_path, max_cases, &scores, &latency, &cases,
                                      progress_path) != 0)
            fatal("LongMemEval eval failed for %s", dataset_path);

         char title[1024];
         snprintf(title, sizeof(title), "LongMemEval Retrieval Evaluation — %s", dataset_path);
         print_mem_eval_report(title, &scores, &latency);
      }
      if (report_misses)
         (void)mem_eval_report_longmemeval_misses(dataset_path, max_cases, miss_limit, max_misses,
                                                  stdout, miss_progress_path);
      return;
   }
   else if (strcmp(argv[0], "locomo-qa") == 0)
   {
      const char *dataset_path = "data/locomo/locomo10.json";
      int max_cases = 0;
      int top_k = 10;
      int token_budget = 2000;
      int report_failures = 0;
      int max_failures = 5;
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc)
            dataset_path = argv[++i];
         else if (strcmp(argv[i], "--max-cases") == 0 && i + 1 < argc)
            max_cases = atoi(argv[++i]);
         else if (strcmp(argv[i], "--top-k") == 0 && i + 1 < argc)
            top_k = atoi(argv[++i]);
         else if (strcmp(argv[i], "--token-budget") == 0 && i + 1 < argc)
            token_budget = atoi(argv[++i]);
         else if (strcmp(argv[i], "--report-failures") == 0)
            report_failures = 1;
         else if (strcmp(argv[i], "--max-failures") == 0 && i + 1 < argc)
            max_failures = atoi(argv[++i]);
      }

      mem_eval_qa_scores_t scores;
      mem_eval_latency_t latency;
      int samples = 0;
      if (mem_eval_run_locomo_qa(dataset_path, max_cases, top_k, token_budget, &scores, &latency,
                                 &samples) != 0)
      {
         fatal("LoCoMo QA eval failed for %s", dataset_path);
      }

      char title[1024];
      snprintf(title, sizeof(title),
               "LoCoMo QA Evaluation — %s (%d conversations, top_k=%d token_budget=%d)",
               dataset_path, samples, top_k, token_budget);
      print_mem_eval_qa_report(title, &scores, &latency);
      if (report_failures)
         (void)mem_eval_report_locomo_qa_failures(dataset_path, max_cases, top_k, token_budget,
                                                  max_failures, stdout);
      return;
   }
   else if (strcmp(argv[0], "longmemeval-qa") == 0)
   {
      const char *dataset_path = "data/longmemeval/longmemeval_s_cleaned.json";
      int max_cases = 0;
      int top_k = 10;
      int token_budget = 2000;
      int report_failures = 0;
      int max_failures = 5;
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc)
            dataset_path = argv[++i];
         else if (strcmp(argv[i], "--max-cases") == 0 && i + 1 < argc)
            max_cases = atoi(argv[++i]);
         else if (strcmp(argv[i], "--top-k") == 0 && i + 1 < argc)
            top_k = atoi(argv[++i]);
         else if (strcmp(argv[i], "--token-budget") == 0 && i + 1 < argc)
            token_budget = atoi(argv[++i]);
         else if (strcmp(argv[i], "--report-failures") == 0)
            report_failures = 1;
         else if (strcmp(argv[i], "--max-failures") == 0 && i + 1 < argc)
            max_failures = atoi(argv[++i]);
      }

      mem_eval_qa_scores_t scores;
      mem_eval_latency_t latency;
      int cases = 0;
      if (mem_eval_run_longmemeval_qa(dataset_path, max_cases, top_k, token_budget, &scores,
                                      &latency, &cases) != 0)
      {
         fatal("LongMemEval QA eval failed for %s", dataset_path);
      }

      char title[1024];
      snprintf(title, sizeof(title),
               "LongMemEval QA Evaluation — %s (%d cases, top_k=%d token_budget=%d)", dataset_path,
               cases, top_k, token_budget);
      print_mem_eval_qa_report(title, &scores, &latency);
      if (report_failures)
         (void)mem_eval_report_longmemeval_qa_failures(dataset_path, max_cases, top_k, token_budget,
                                                       max_failures, stdout);
      return;
   }
   else if (strcmp(argv[0], "run") == 0)
   {
      if (argc < 2)
         fatal("usage: aimee eval run <suite_dir> [--ablation <preset|all>] [--runs N]");

      agent_eval_run_options_t eval_opts;
      memset(&eval_opts, 0, sizeof(eval_opts));
      eval_opts.ablation = "full";
      eval_opts.runs = 1;
      eval_opts.seed = 42;
      for (int i = 2; i < argc; i++)
      {
         if (strcmp(argv[i], "--ablation") == 0 && i + 1 < argc)
            eval_opts.ablation = argv[++i];
         else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
            eval_opts.runs = atoi(argv[++i]);
         else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            eval_opts.seed = (unsigned int)strtoul(argv[++i], NULL, 10);
         else
            fatal("usage: aimee eval run <suite_dir> [--ablation <preset|all>] [--runs N]");
      }
      if (eval_opts.runs <= 0)
         eval_opts.runs = 1;
      if (strcmp(eval_opts.ablation, "all") != 0)
      {
         agent_ablation_flags_t check_flags;
         if (agent_eval_ablation_preset(eval_opts.ablation, &check_flags) != 0)
            fatal("eval run: unknown ablation preset '%s'", eval_opts.ablation);
      }

      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
         fatal("no agents configured");

      agent_http_init();
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("eval run: could not initialize DB1");
      eval_result_t results[AGENT_MAX_EVAL_TASKS];
      int passes =
          agent_eval_run_with_options(&acfg, argv[1], &eval_opts, results, AGENT_MAX_EVAL_TASKS);

      /* Load task count for reporting */
      eval_task_t tasks[AGENT_MAX_EVAL_TASKS];
      int total = agent_eval_load_tasks(argv[1], tasks, AGENT_MAX_EVAL_TASKS);
      int preset_count = strcmp(eval_opts.ablation, "all") == 0 ? 7 : 1;
      int rows = total * preset_count * eval_opts.runs;
      if (rows > AGENT_MAX_EVAL_TASKS)
         rows = AGENT_MAX_EVAL_TASKS;

      printf("%-30s %-12s %-12s %-6s %-6s %-8s %-10s\n", "Task", "Agent", "Ablation", "Pass",
             "Turns", "ToolOK", "Latency");
      for (int i = 0; i < rows; i++)
      {
         printf("%-30s %-12s %-12s %-6s %-6d %-7.2f%% %-10dms\n", results[i].task_name,
                results[i].agent_name, results[i].ablation, results[i].success ? "PASS" : "FAIL",
                results[i].turns, results[i].tool_call_success_rate * 100.0, results[i].latency_ms);
      }
      printf("\n%d/%d passed.\n", passes, total * preset_count * eval_opts.runs);

      agent_http_cleanup();
   }
   else if (strcmp(argv[0], "results") == 0)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("eval results: could not initialize DB1");
      const char *suite_filter = (argc >= 2) ? argv[1] : NULL;

      db1_eval_display_row_t rows[50];
      int nrows = db1_eval_results_list(suite_filter, rows, 50);
      if (nrows > 0)
      {
         printf("%-15s %-25s %-12s %-12s %-6s %-6s %-6s %-10s %s\n", "Suite", "Task", "Agent",
                "Ablation", "Pass", "Turns", "Tools", "Latency", "Time");
         for (int i = 0; i < nrows; i++)
            printf("%-15s %-25s %-12s %-12s %-6s %-6d %-6d %-10dms %s\n", rows[i].suite,
                   rows[i].task_name, rows[i].agent_name, rows[i].ablation,
                   rows[i].success ? "PASS" : "FAIL", rows[i].turns, rows[i].tool_calls,
                   rows[i].latency_ms, rows[i].created_at);
      }
   }
}
