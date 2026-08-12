/* cmd_provider.c: aimee provider — list, show, and model catalog. */
#include "model_provider.h"
#include "model_registry.h"
#include "models_dev.h"
#include "aimee.h"
#include "commands.h"
#include "config.h"
#include "db1.h"
#include "runtime_secret.h"
#include <aimee/delegates/delegate_credentials.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *provider_first_env_var(const model_provider_t *p)
{
   if (!p || !p->env_vars)
      return NULL;
   for (int i = 0; p->env_vars[i]; i++)
   {
      if (p->env_vars[i][0])
         return p->env_vars[i];
   }
   return NULL;
}

static int provider_has_credentials(const model_provider_t *p)
{
   if (!p)
      return 0;
   if (p->auth_type && strcmp(p->auth_type, "none") == 0)
      return 1;
   if (!p->env_vars)
      return 0;
   for (int i = 0; p->env_vars[i]; i++)
   {
      if (runtime_secret_has(p->env_vars[i]))
         return 1;
   }
   return 0;
}

static void provider_free_models(provider_model_t *models, int n)
{
   db1_model_catalog_free(models, n);
}

#define PROVIDER_MODEL_CATALOG_TTL_SECONDS 3600

static int provider_models_cached(model_provider_t *p, provider_model_t **models_out, int *n_out)
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

/* Has the OPERATOR configured this provider, as opposed to aimee merely knowing
 * it exists? A credential or an explicit `provider set` is evidence of a
 * deliberate choice. auth_type "none" alone (ollama, llama_native) is NOT:
 * those need no key, so they would otherwise report configured on a machine
 * where nothing is installed or running. */
static int provider_is_configured(const model_provider_t *p, const char *selected_provider)
{
   if (!p)
      return 0;
   if (selected_provider && selected_provider[0] && strcmp(p->name, selected_provider) == 0)
      return 1;
   if (!p->env_vars)
      return 0;
   for (int i = 0; p->env_vars[i]; i++)
   {
      if (runtime_secret_has(p->env_vars[i]))
         return 1;
   }
   return 0;
}

static void provider_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   int only_available = 0, show_all = 0;
   for (int ai = 0; ai < argc; ai++)
   {
      if (strcmp(argv[ai], "--available") == 0)
         only_available = 1;
      else if (strcmp(argv[ai], "--all") == 0)
         show_all = 1;
   }

   model_provider_t *providers[64];
   int n = model_provider_list(providers, 64);

   /* Default: only what the operator configured. This used to print the whole
    * built-in catalogue — seven rows, every one "[no key]" — under a heading
    * that reads like a registration table, so a brand-new install looked
    * pre-populated with providers nobody had chosen. Nothing was installed or
    * configured; they were just names aimee knows. `--all` shows that catalogue
    * for anyone who wants to see what can be configured, and `--available`
    * still answers the different question of what is usable right now
    * (including the local, no-key providers). */
   int shown = 0;
   for (int i = 0; i < n; i++)
   {
      const model_provider_t *p = providers[i];
      int available = provider_has_credentials(p);
      if (only_available && !available)
         continue;
      if (!show_all && !only_available && !provider_is_configured(p, config_provider()))
         continue;
      const char *key_var = provider_first_env_var(p);
      printf("%-20s  %-24s  %-10s  %s\n", p->name, p->display_name ? p->display_name : "",
             p->auth_type ? p->auth_type : "",
             key_var ? (available ? "[key set]" : "[no key]") : "[local]");
      shown++;
   }

   if (shown == 0)
   {
      if (show_all)
         printf("no providers known\n");
      else if (only_available)
         printf("no providers available\n");
      else
         printf("no providers configured — `aimee provider list --all` shows what can be\n");
   }
}

static void provider_show(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee provider show <name>\n");
      return;
   }
   model_provider_t *p = model_provider_get(argv[0]);
   if (!p)
   {
      fprintf(stderr, "provider '%s' not found\n", argv[0]);
      return;
   }
   printf("name:          %s\n", p->name);
   printf("display_name:  %s\n", p->display_name ? p->display_name : "");
   printf("description:   %s\n", p->description ? p->description : "");
   printf("base_url:      %s\n", p->base_url ? p->base_url : "");
   printf("models_url:    %s\n", p->models_url ? p->models_url : "");
   printf("auth_type:     %s\n", p->auth_type ? p->auth_type : "");
   printf("default_model: %s\n", p->default_model ? p->default_model : "");
   printf("aux_model:     %s\n", p->default_aux_model ? p->default_aux_model : "");
   if (p->env_vars)
   {
      for (int i = 0; p->env_vars[i]; i++)
      {
         printf("env[%d]:        %s=%s\n", i, p->env_vars[i],
                runtime_secret_has(p->env_vars[i]) ? "[vaulted]" : "[unset]");
      }
   }
}

static void provider_models(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee provider models <name>\n");
      return;
   }
   int json_output = 0;
   for (int ai = 1; ai < argc; ai++)
   {
      if (strcmp(argv[ai], "--json") == 0)
         json_output = 1;
   }

   model_provider_t *p = model_provider_get(argv[0]);
   if (!p)
   {
      fprintf(stderr, "provider '%s' not found\n", argv[0]);
      return;
   }
   if (!p->fetch_models)
   {
      fprintf(stderr, "provider '%s' does not support model listing\n", p->name);
      return;
   }

   provider_model_t *models = NULL;
   int n = 0;
   if (!json_output)
      fprintf(stderr, "fetching models from %s...\n", p->models_url ? p->models_url : p->name);
   if (provider_models_cached(p, &models, &n) != 0)
   {
      fprintf(stderr, "failed to fetch models (is %s set?)\n",
              provider_first_env_var(p) ? provider_first_env_var(p) : "the API key");
      return;
   }
   if (json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(models[i].id));
      char *json = cJSON_Print(arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(arr);
      provider_free_models(models, n);
      return;
   }
   for (int i = 0; i < n; i++)
      printf("%s\n", models[i].id);
   provider_free_models(models, n);
   fprintf(stderr, "%d models\n", n);
}

static void provider_test(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee provider test <name>\n");
      return;
   }
   model_provider_t *p = model_provider_get(argv[0]);
   if (!p)
   {
      fprintf(stderr, "provider '%s' not found\n", argv[0]);
      return;
   }
   if (!provider_has_credentials(p))
   {
      fprintf(stderr, "provider '%s' unavailable (missing %s)\n", p->name,
              provider_first_env_var(p) ? provider_first_env_var(p) : "credentials");
      return;
   }
   if (!p->fetch_models)
   {
      printf("%s: credentials available; no model-listing probe registered\n", p->name);
      return;
   }

   provider_model_t *models = NULL;
   int n = 0;
   if (p->fetch_models(p, &models, &n) != 0)
   {
      fprintf(stderr, "%s: probe failed\n", p->name);
      return;
   }
   provider_free_models(models, n);
   printf("%s: ok (%d models)\n", p->name, n);
}

static void provider_quota(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   const char *name = (argc >= 1) ? argv[0] : NULL;
   char state_path[MAX_PATH_LEN];
   const char *dir = config_output_dir();
   snprintf(state_path, sizeof(state_path), "%s/delegate-credential-state.tsv", dir ? dir : "/tmp");
   (void)delegate_credentials_load_file(state_path, time(NULL));
   char buf[4096];
   int n = delegate_credentials_format_quota(name, buf, sizeof(buf));
   if (n < 0)
   {
      fprintf(stderr, "failed to format credential quota state\n");
      return;
   }
   printf("%s", buf);
}

static const subcmd_t provider_subcmds[] = {
    {"list", "List configured providers (--all: every known one, --available: usable now)",
     provider_list},
    {"show", "Show provider profile detail", provider_show},
    {"models", "Fetch live model catalog from provider", provider_models},
    {"test", "Probe provider connectivity", provider_test},
    {"quota", "Show process-local credential pool quota state", provider_quota},
    {NULL, NULL, NULL},
};

void cmd_provider(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      provider_list(ctx, 0, NULL);
      return;
   }
   subcmd_dispatch(provider_subcmds, argv[0], ctx, argc - 1, argv + 1);
}

static void model_print_capability(const model_capability_t *cap)
{
   char flags[128];
   model_capability_flags_string(cap->flags, flags, sizeof(flags));
   printf("provider:         %s\n", cap->provider);
   printf("model:            %s\n", cap->model_id);
   printf("context_window:   %d\n", cap->context_window);
   printf("max_output:       %d\n", cap->max_output);
   printf("cost_in_per_mtok: %.4f\n", cap->cost_in_per_mtok);
   printf("cost_out_per_mtok: %.4f\n", cap->cost_out_per_mtok);
   printf("capabilities:     %s\n", flags);
   printf("modalities:       %s\n", cap->modalities[0] ? cap->modalities : "text");
   printf("knowledge_cutoff: %s\n", cap->knowledge_cutoff[0] ? cap->knowledge_cutoff : "unknown");
   printf("open_weights:     %s\n", cap->open_weights ? "true" : "false");
   printf("deprecated:       %s\n", cap->deprecated ? "true" : "false");
}

static void model_show(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee catalog show [provider:]<model>\n");
      return;
   }

   char provider[MODEL_PROVIDER_MAX] = "";
   const char *model = argv[0];
   const char *colon = strchr(argv[0], ':');
   if (colon && colon != argv[0] && colon[1])
   {
      size_t plen = (size_t)(colon - argv[0]);
      if (plen >= sizeof(provider))
         plen = sizeof(provider) - 1;
      memcpy(provider, argv[0], plen);
      provider[plen] = '\0';
      model = colon + 1;
   }

   model_capability_t cap;
   if (!model_capability_get(provider[0] ? provider : NULL, model, &cap))
   {
      fprintf(stderr, "model capability metadata not found for '%s'\n", argv[0]);
      return;
   }
   model_print_capability(&cap);
}

static void model_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   unsigned required = 0;
   int open_weights_only = 0;
   for (int ai = 0; ai < argc; ai++)
   {
      if (strcmp(argv[ai], "--capability") == 0 && ai + 1 < argc)
      {
         unsigned flag = model_capability_flag_from_name(argv[++ai]);
         if (!flag)
         {
            fprintf(stderr, "unknown capability '%s'\n", argv[ai]);
            return;
         }
         required |= flag;
      }
      else if (strcmp(argv[ai], "--open-weights") == 0)
         open_weights_only = 1;
      else
      {
         fprintf(stderr, "usage: aimee catalog list [--capability <name>] [--open-weights]\n");
         return;
      }
   }

   model_capability_t caps[128];
   int total = model_capability_list(caps, 128, required, open_weights_only);
   int n = total < 128 ? total : 128;
   for (int i = 0; i < n; i++)
   {
      char flags[128];
      model_capability_flags_string(caps[i].flags, flags, sizeof(flags));
      printf("%-12s %-32s ctx=%-8d flags=%s%s\n", caps[i].provider, caps[i].model_id,
             caps[i].context_window, flags, caps[i].deprecated ? " deprecated" : "");
   }
   if (total > n)
      fprintf(stderr, "%d additional model(s) omitted\n", total - n);
}

static void model_refresh(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   char msg[256] = "";
   int n = model_capability_refresh(msg, sizeof(msg));
   int net_rc = models_dev_refresh();
   if (msg[0])
      printf("%s\n", msg);
   else if (n > 0)
      printf("model metadata refreshed (%d entries)\n", n);
   else if (net_rc == 0)
      printf("model metadata refreshed\n");
   else
      printf("model metadata refresh failed (network unavailable; using cached snapshot)\n");
}

static const subcmd_t model_subcmds[] = {
    {"show", "Show model capability metadata", model_show},
    {"list", "List known model capability metadata", model_list},
    {"refresh", "Refresh model metadata cache", model_refresh},
    {NULL, NULL, NULL},
};

void cmd_model(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      model_list(ctx, 0, NULL);
      return;
   }
   subcmd_dispatch(model_subcmds, argv[0], ctx, argc - 1, argv + 1);
}
