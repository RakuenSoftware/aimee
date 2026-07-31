/* model_sampling.c: curated opt-in sampling defaults for local delegates */
#include "model_sampling.h"
#include "util.h"
#include "model_provider.h"
#include "cJSON.h"
#include <ctype.h>
#include <string.h>

static const model_sampling_row_t g_sampling_rows[] = {
    {"ministral-3", 0.05, -1, -1, -1, -1,
     "https://huggingface.co/mistralai/Ministral-3-8B-Instruct-2512"},
    {"ministral", 0.05, -1, -1, -1, -1,
     "https://huggingface.co/mistralai/Ministral-3-8B-Instruct-2512"},
    {"qwen3", 0.6, 0.95, 20, 0.0, -1, "https://huggingface.co/Qwen/Qwen3-8B"},
    {"qwen_qwen3", 0.6, 0.95, 20, 0.0, -1, "https://huggingface.co/Qwen/Qwen3-8B"},
    {"gemma-4", 1.0, -1, 64, -1, -1, "https://huggingface.co/google/gemma-4-26b-it"},
    {"gemma4", 1.0, -1, 64, -1, -1, "https://huggingface.co/google/gemma-4-26b-it"},
    {"granite", 0.0, -1, -1, -1, -1, "https://huggingface.co/ibm-granite"},
    {"mistral-small", 0.15, -1, -1, -1, -1,
     "https://huggingface.co/mistralai/Mistral-Small-3.2-24B-Instruct-2506"},
    {NULL, -1, -1, -1, -1, -1, NULL},
};

int model_sampling_get(const char *model_key, model_sampling_row_t *out)
{
   if (!model_key || !model_key[0])
      return 0;
   for (int i = 0; g_sampling_rows[i].model_key; i++)
   {
      if (str_contains_ci(model_key, g_sampling_rows[i].model_key))
      {
         if (out)
            *out = g_sampling_rows[i];
         return 1;
      }
   }
   return 0;
}

/* Models that accept exactly one temperature and 4xx on anything else.
 * |vendor| is the catalog vendor (agent_t.catalog_provider), which is derived
 * from the endpoint host, so this still applies to an agents.json entry that
 * names no wire provider at all — the case that made kimi-k3 fail every
 * delegate with "invalid temperature: only 1 is allowed for this model". */
static const struct
{
   const char *vendor;
   const char *model_key;
   double temperature;
} g_required_temperature[] = {
    {"moonshotai", "k3", 1.0},
    {"mistral", "mistral-vibe-cli", 1.0},
    {NULL, NULL, -1},
};

double model_sampling_required_temperature(const agent_t *agent)
{
   if (!agent || !agent->model[0])
      return -1;
   for (int i = 0; g_required_temperature[i].model_key; i++)
   {
      const char *vendor = g_required_temperature[i].vendor;
      if (vendor && strcmp(agent->catalog_provider, vendor) != 0 &&
          strcmp(agent->provider, vendor) != 0)
         continue;
      if (str_contains_ci(agent->model, g_required_temperature[i].model_key))
         return g_required_temperature[i].temperature;
   }
   return -1;
}

static double provider_fixed_temperature(const agent_t *agent)
{
   if (!agent || !agent->provider[0])
      return -1;
   model_provider_t *provider = model_provider_get(agent->provider);
   if (!provider || provider->fixed_temperature < 0)
      return -1;
   return (double)provider->fixed_temperature;
}

static void add_number_if_missing(struct cJSON *req, const char *name, double value)
{
   if (!req || !name || value < 0 || cJSON_GetObjectItem(req, name))
      return;
   cJSON_AddNumberToObject(req, name, value);
}

static void add_int_if_missing(struct cJSON *req, const char *name, int value)
{
   if (!req || !name || value < 0 || cJSON_GetObjectItem(req, name))
      return;
   cJSON_AddNumberToObject(req, name, value);
}

static int sampling_for_agent(const agent_t *agent, model_sampling_row_t *row)
{
   if (!agent || !agent->recommended_sampling)
      return 0;
   return model_sampling_get(agent->model, row);
}

void model_sampling_apply_openai(const agent_t *agent, struct cJSON *req, double caller_temperature)
{
   model_sampling_row_t row;
   int has_row = sampling_for_agent(agent, &row);

   /* First writer wins (add_number_if_missing), so the model's hard constraint
    * goes in ahead of the caller's preference rather than losing to it. */
   add_number_if_missing(req, "temperature", model_sampling_required_temperature(agent));
   if (caller_temperature >= 0)
      add_number_if_missing(req, "temperature", caller_temperature);
   else if (has_row && row.temperature >= 0)
      add_number_if_missing(req, "temperature", row.temperature);
   else
      add_number_if_missing(req, "temperature", provider_fixed_temperature(agent));

   if (!has_row)
      return;
   add_number_if_missing(req, "top_p", row.top_p);
   add_int_if_missing(req, "top_k", row.top_k);
   add_number_if_missing(req, "min_p", row.min_p);
   add_number_if_missing(req, "repeat_penalty", row.repeat_penalty);
}

void model_sampling_apply_anthropic(const agent_t *agent, struct cJSON *req,
                                    double caller_temperature)
{
   model_sampling_row_t row;
   int has_row = sampling_for_agent(agent, &row);

   add_number_if_missing(req, "temperature", model_sampling_required_temperature(agent));
   if (caller_temperature >= 0)
      add_number_if_missing(req, "temperature", caller_temperature);
   else if (has_row && row.temperature >= 0)
      add_number_if_missing(req, "temperature", row.temperature);
   else
      add_number_if_missing(req, "temperature", provider_fixed_temperature(agent));

   if (!has_row)
      return;
   add_number_if_missing(req, "top_p", row.top_p);
   add_int_if_missing(req, "top_k", row.top_k);
}
