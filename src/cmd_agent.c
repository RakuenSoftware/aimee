/* cmd_agent.c: agent subcommand CLI (agent list/test/run/add/remove/setup/token) */
#include "aimee.h"
#include "util.h"
#include "db1.h"
#include "agent.h"
#include "agent_config.h"
#include "agent_tier_lint.h" /* agent_resolved_price */
#include "model_registry.h"
#include <math.h>
#include <errno.h>
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
   ag_set_roles_csv(ag, "code,explain,refactor,draft,execute,summarize,format,reason,search");
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

/* Strict $/Mtok parse: finite, non-negative, fully consumed. Rejects "nan",
 * "inf", trailing junk, and the empty string. atof() would accept or silently
 * zero all of these, and a NaN override defeats every `<=` comparison in the
 * price resolver, making an unset price look set. */
static int ag_parse_price(const char *s, double *out)
{
   if (!s || !s[0])
      return 0;
   errno = 0;
   char *end = NULL;
   double v = strtod(s, &end);
   if (errno != 0 || end == s || (end && *end != '\0'))
      return 0;
   if (!isfinite(v) || v < 0.0)
      return 0;
   *out = v;
   return 1;
}

static int ag_price_usage(const char *flag, const char *value)
{
   fprintf(stderr,
           "aimee: %s expects a finite non-negative number ($ per million tokens), got '%s'\n",
           flag, value ? value : "");
   return 1;
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
   int rc = config_set_model_concurrency(model, limit);
   return rc == -2 ? -1 : rc; /* table full kept as the caller's -1 */
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

   return config_remove_model_concurrency(model);
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
         /* Vendor identity used for capability/price lookup, which differs from
          * `provider` (the wire shape) for a third-party model served over
          * another vendor's API. Surfaced so the GUI can show provider+model. */
         cJSON_AddStringToObject(obj, "catalog_provider", agent_catalog_provider(ag));
         /* Canonical `provider:model` reference (the form model_capability_resolve_ref
          * parses and `aimee catalog show` accepts) plus the catalog's human label,
          * so any surface that must name a SPECIFIC model — roundtable seats,
          * routing attribution, a picker — can show provider+model without
          * hand-maintained strings. display_name is omitted when the catalog has
          * none rather than echoing the id, so a consumer can tell them apart. */
         if (ag->model[0])
         {
            char ref[MODEL_PROVIDER_MAX + MAX_MODEL_LEN + 2];
            snprintf(ref, sizeof(ref), "%s:%s", agent_catalog_provider(ag), ag->model);
            cJSON_AddStringToObject(obj, "model_ref", ref);
            model_capability_t dcap;
            if (model_capability_get(agent_catalog_provider(ag), ag->model, &dcap) &&
                dcap.display_name[0])
               cJSON_AddStringToObject(obj, "model_display_name", dcap.display_name);
         }
         cJSON_AddNumberToObject(obj, "cost_tier", ag->cost_tier);
         /* Effective price ($/Mtok): operator override when set, else catalog.
          * Emitted only when BOTH axes resolve, so a consumer never reads an
          * unknown price as free. `price_overridden` tells the GUI whether the
          * operator pinned it or it came from the catalog. */
         {
            double pin = 0.0, pout = 0.0, pcached = 0.0;
            if (agent_resolved_price(ag, &pin, &pout, &pcached))
            {
               /* BASE-BAND rate. Named so a consumer cannot mistake it for the
                * effective price of a large request: several providers charge
                * more above a context threshold, and this figure is only correct
                * below the first band. `price_bands` carries the rest. */
               cJSON_AddNumberToObject(obj, "price_base_in_per_mtok", pin);
               cJSON_AddNumberToObject(obj, "price_base_out_per_mtok", pout);
               /* Omitted entirely when unpublished, so a consumer cannot mistake
                * an absent cache rate for a free one. */
               if (pcached > 0.0)
                  cJSON_AddNumberToObject(obj, "price_base_cached_per_mtok", pcached);
               /* DEPRECATED aliases, retained so the rename is not a silent
                * machine-interface break for existing `agent list --json`
                * consumers. They carry the BASE-band rate; read price_bands to
                * price a large request. */
               cJSON_AddNumberToObject(obj, "price_in_per_mtok", pin);
               cJSON_AddNumberToObject(obj, "price_out_per_mtok", pout);
               if (pcached > 0.0)
                  cJSON_AddNumberToObject(obj, "price_cached_per_mtok", pcached);

               model_capability_t cap;
               if (ag->model[0] &&
                   model_capability_get(agent_catalog_provider(ag), ag->model, &cap) &&
                   cap.price_band_count > 0)
               {
                  cJSON *bands = cJSON_AddArrayToObject(obj, "price_bands");
                  for (int b = 0; bands && b < cap.price_band_count; b++)
                  {
                     double bin = 0.0, bout = 0.0, bcached = 0.0;
                     int above = cap.price_bands[b].above_tokens;
                     if (!agent_resolved_price_at_context(ag, above + 1, &bin, &bout, &bcached))
                        continue;
                     cJSON *e = cJSON_CreateObject();
                     if (!e)
                        continue;
                     cJSON_AddNumberToObject(e, "above_tokens", above);
                     cJSON_AddNumberToObject(e, "in_per_mtok", bin);
                     cJSON_AddNumberToObject(e, "out_per_mtok", bout);
                     if (bcached > 0.0)
                        cJSON_AddNumberToObject(e, "cached_per_mtok", bcached);
                     cJSON_AddItemToArray(bands, e);
                  }
               }
            }
            cJSON_AddBoolToObject(obj, "price_overridden",
                                  ag->price_in_per_mtok > 0.0 || ag->price_out_per_mtok > 0.0 ||
                                      ag->price_cached_per_mtok > 0.0);
         }
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
         double pin = 0.0, pout = 0.0, pcached = 0.0;
         char price[96] = "";
         if (agent_resolved_price(ag, &pin, &pout, &pcached))
         {
            char cached[32] = "";
            if (pcached > 0.0)
               snprintf(cached, sizeof(cached), " cached=$%.2f", pcached);
            snprintf(price, sizeof(price), " base in=$%.2f out=$%.2f%s%s", pin, pout, cached,
                     (ag->price_in_per_mtok > 0.0 || ag->price_out_per_mtok > 0.0 ||
                      ag->price_cached_per_mtok > 0.0)
                         ? " *"
                         : "");
         }
         printf("%-16s %-6s tier=%d parallel=%d model=%s endpoint=%s%s%s\n", ag->name,
                ag->enabled ? "ON" : "OFF", ag->cost_tier, ag->max_parallel, ag->model,
                ag->endpoint, ag->tools_enabled ? " [tools]" : "", price);
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
   int ok = agent_run_parallel(&s_agent_cfg, tasks, n, results, 0 /* no deadline */);
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
   if (db1_init(config_db1_path()) != 0)
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

   if (!agent_name_valid(argv[0]))
      fatal("invalid agent name '%s': use 1–48 chars, starting alphanumeric, then "
            "alphanumeric or . _ -",
            argv[0]);

   /* The endpoint is positional, so a flag typed where it belongs is silently
    * ACCEPTED as the address: `agent add x --provider openai --endpoint URL` stored
    * endpoint="--provider", saved, reported the agent ON, and exited 0. The failure
    * surfaced only at `agent probe`, as "GET --provider/models returned -1".
    *
    * Only a leading '-' is refused. That is unambiguous evidence of a mis-parsed
    * flag — no address starts with one — whereas demanding a scheme would reject
    * host:port forms this command has never rejected before. */
   if (!agent_endpoint_valid(argv[1]))
      fatal("'%s' is not an endpoint — it looks like a flag in the endpoint's place.\n"
            "  usage: aimee agent add <name> <endpoint> <model> [options]\n"
            "  the first three arguments are positional, e.g.\n"
            "    aimee agent add local http://127.0.0.1:8080/v1 my-model",
            argv[1]);

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

   int roles_specified = 0;
   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
      {
         /* Store the verbatim reference (e.g. "$VAR") in BOTH fields and resolve
          * nothing here. Mirrors agent_load_config, which no longer resolves
          * either: a resolved secret must never be written to agents.json, and
          * must never sit in the in-memory registry. agent_api_key_secret()
          * resolves at the point of use. */
         const char *raw_key = argv[++i];
         snprintf(ag->api_key_disk, MAX_API_KEY_LEN, "%s", raw_key);
         snprintf(ag->api_key, MAX_API_KEY_LEN, "%s", raw_key);
      }
      else if (strcmp(argv[i], "--auth-cmd") == 0 && i + 1 < argc)
         snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", argv[++i]);
      else if (strcmp(argv[i], "--auth-type") == 0 && i + 1 < argc)
         snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", argv[++i]);
      else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
         snprintf(ag->provider, sizeof(ag->provider), "%s", argv[++i]);
      else if (strcmp(argv[i], "--roles") == 0 && i + 1 < argc)
      {
         roles_specified = 1;
         ag_set_roles_csv(ag, argv[++i]);
      }
      else if (strcmp(argv[i], "--cost-tier") == 0 && i + 1 < argc)
         ag->cost_tier = atoi(argv[++i]);
      /* Price overrides ($/Mtok). Only meaningful when this deployment does not
       * pay the published catalog rate. Parsed strictly: atof() would turn
       * "garbage" into 0 (silently meaning "unset") and would accept "nan" and
       * "inf", which then defeat every ordered comparison downstream. */
      else if (strcmp(argv[i], "--price-in") == 0 && i + 1 < argc)
      {
         if (!ag_parse_price(argv[++i], &ag->price_in_per_mtok))
         {
            ag_price_usage("--price-in", argv[i]);
            return;
         }
         /* Naming the flag is a declaration, including "--price-in 0" for a
          * free or subscription-priced seat. */
         ag->declared |= AGENT_DECL_PRICE_IN;
      }
      else if (strcmp(argv[i], "--price-out") == 0 && i + 1 < argc)
      {
         if (!ag_parse_price(argv[++i], &ag->price_out_per_mtok))
         {
            ag_price_usage("--price-out", argv[i]);
            return;
         }
         /* Naming the flag is a declaration, including "--price-out 0" for a
          * free or subscription-priced seat. */
         ag->declared |= AGENT_DECL_PRICE_OUT;
      }
      else if (strcmp(argv[i], "--price-cached") == 0 && i + 1 < argc)
      {
         if (!ag_parse_price(argv[++i], &ag->price_cached_per_mtok))
         {
            ag_price_usage("--price-cached", argv[i]);
            return;
         }
         /* Naming the flag is a declaration, including "--price-cached 0" for a
          * free or subscription-priced seat. */
         ag->declared |= AGENT_DECL_PRICE_CACHED;
      }
      else if (strcmp(argv[i], "--tools-enabled") == 0 || strcmp(argv[i], "--tools") == 0)
         ag->tools_enabled = 1;
      else if (strcmp(argv[i], "--max-turns") == 0 && i + 1 < argc)
         ag->max_turns = atoi(argv[++i]);
      else if (strcmp(argv[i], "--max-parallel") == 0 && i + 1 < argc)
         ag->max_parallel = atoi(argv[++i]);
      else if ((strcmp(argv[i], "--ctx") == 0 || strcmp(argv[i], "--context-window") == 0) &&
               i + 1 < argc)
      {
         ag->middleware.context_window = atoi(argv[++i]);
         ag->declared |= AGENT_DECL_CONTEXT_WINDOW;
      }
      else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc)
         ag->max_tokens = atoi(argv[++i]);
      /* The model's output ceiling, distinct from --max-tokens (what this
       * deployment asks for per request). Declared, so 0 is a real value. */
      else if (strcmp(argv[i], "--max-output") == 0 && i + 1 < argc)
      {
         ag->max_output = atoi(argv[++i]);
         ag->declared |= AGENT_DECL_MAX_OUTPUT;
      }
      else if ((strcmp(argv[i], "--timeout-ms") == 0 || strcmp(argv[i], "--timeout") == 0) &&
               i + 1 < argc)
         ag->timeout_ms = atoi(argv[++i]);
      else if (strcmp(argv[i], "--exec-roles") == 0 && i + 1 < argc)
         ag_set_exec_roles_csv(ag, argv[++i]);
   }

   /* Preserve an explicitly empty --roles selection. Omission gets the small
    * historical add default, which does not silently authorize review. */
   if (!roles_specified)
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

/* Surgically update ONLY an agent's roles, preserving endpoint/model/provider/
 * auth/vault key (unlike `agent add`, which resets the whole record). Fixes the
 * config regression where capable coding delegates were left with just
 * summarize/format/draft. Omit the csv to SHOW the roles; `--reset` restores the
 * default delegate set. Review is an explicit operator grant. */
static void ag_roles(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
      fatal("usage: aimee agent roles <name> [role1,role2,... | --reset]  "
            "(omit to show the current roles)");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);
   /* No csv shows the roles and writes nothing — see handle_agent_roles. */
   if (argc < 2 || !argv[1] || !argv[1][0])
   {
      printf("Agent '%s' roles:", ag->name);
      for (int i = 0; i < ag->role_count; i++)
         printf(" %s", ag->roles[i]);
      printf("\n");
      return;
   }
   if (strcmp(argv[1], "--reset") == 0)
      ag_set_default_delegate_roles(ag);
   else
      ag_set_roles_csv(ag, argv[1]);
   agent_save_config(&s_agent_cfg);
   printf("Agent '%s' roles set to:", ag->name);
   for (int i = 0; i < ag->role_count; i++)
      printf(" %s", ag->roles[i]);
   printf("\n");
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

   if (roles_arg)
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
    {"roles", "Show delegate roles, or set them (csv, or --reset for defaults)", ag_roles},
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
      if (db1_init(config_db1_path()) != 0)
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
      if (db1_init(config_db1_path()) != 0)
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
      if (db1_init(config_db1_path()) != 0)
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
      if (db1_init(config_db1_path()) != 0)
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

/* cmd_eval moved to the benchmarks module (modules/benchmarks/agent_eval_suite_cli.c)
 * and is reached through `aimee-server --eval`. Benchmarks are dev-only and
 * server-side. The `run` and `results` subcommands were dropped rather than
 * moved: they needed DB1 and the agent runtime, which the benchmarks module does
 * not depend on, and they duplicated the live eval.run / eval.results server
 * routes (server/server.c:1567-1568). */
