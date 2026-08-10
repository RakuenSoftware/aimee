/* anthropic_profile.c: model_provider_t profile for Anthropic. */
#include "model_provider.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "model_registry.h"
#include "provider_model_parse.h"
#include "runtime_secret.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *anthropic_env_vars[] = {"ANTHROPIC_API_KEY", NULL};

/* 1 when capabilities.<path...>.supported is true. Walks a chain of object keys
 * terminated by NULL, so a missing intermediate is simply "not published"
 * rather than a crash — the tree gains keys between API revisions. */
static int cap_supported(cJSON *caps, const char *const *path)
{
   cJSON *node = caps;
   for (int i = 0; path[i]; i++)
   {
      if (!cJSON_IsObject(node))
         return 0;
      node = cJSON_GetObjectItemCaseSensitive(node, path[i]);
   }
   if (!cJSON_IsObject(node))
      return 0;
   return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "supported"));
}

/* Anthropic is the one configured provider that publishes real per-model
 * limits and capabilities, so its fetch populates them rather than returning
 * bare ids. This is what makes an Anthropic model's context window and output
 * ceiling self-maintaining instead of transcribed into a table by hand. */
static int anthropic_fetch_models(model_provider_t *p, provider_model_t **models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   char key[384];
   if (!runtime_secret_get("ANTHROPIC_API_KEY", key, sizeof(key)))
      return -1;

   /* Two headers, newline-separated: send_request() splits on '\n'. */
   char headers[512];
   snprintf(headers, sizeof(headers), "x-api-key: %s\nanthropic-version: 2023-06-01", key);

   /* limit=1000 takes the whole catalogue in one page. The endpoint paginates
    * (has_more / last_id); a single page is deliberate rather than an
    * oversight — Anthropic publishes far fewer models than that, and a partial
    * first page would silently under-report. If has_more is ever true here, the
    * follow-up pages are worth adding. */
   char *body = NULL;
   int status =
       agent_http_get("https://api.anthropic.com/v1/models?limit=1000", headers, &body, 15000);
   runtime_secret_wipe(key, sizeof(key));
   runtime_secret_wipe(headers, sizeof(headers));
   if (status != 200 || !body)
   {
      free(body);
      return -1;
   }

   cJSON *root = cJSON_Parse(body);
   free(body);
   if (!root)
      return -1;

   cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
   if (!cJSON_IsArray(data))
   {
      cJSON_Delete(root);
      return -1;
   }

   int n = cJSON_GetArraySize(data);
   if (n <= 0)
   {
      cJSON_Delete(root);
      return -1;
   }
   provider_model_t *rows = calloc((size_t)n, sizeof(*rows));
   if (!rows)
   {
      cJSON_Delete(root);
      return -1;
   }

   static const char *const path_vision[] = {"image_input", NULL};
   static const char *const path_thinking[] = {"thinking", NULL};
   static const char *const path_adaptive[] = {"thinking", "types", "adaptive", NULL};
   static const char *const path_pdf[] = {"pdf_input", NULL};

   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, data)
   {
      if (!cJSON_IsObject(item))
         continue;
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
      if (!id || !id[0])
         continue;

      provider_model_t *m = &rows[count];
      snprintf(m->id, sizeof(m->id), "%s", id);
      const char *disp =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "display_name"));
      if (disp && disp[0])
         snprintf(m->display_name, sizeof(m->display_name), "%s", disp);

      cJSON *ctx = cJSON_GetObjectItemCaseSensitive(item, "max_input_tokens");
      if (cJSON_IsNumber(ctx) && ctx->valuedouble > 0)
         m->context_window = (int)ctx->valuedouble;
      cJSON *out = cJSON_GetObjectItemCaseSensitive(item, "max_tokens");
      if (cJSON_IsNumber(out) && out->valuedouble > 0)
         m->max_output = (int)out->valuedouble;

      cJSON *caps = cJSON_GetObjectItemCaseSensitive(item, "capabilities");
      if (cJSON_IsObject(caps))
      {
         if (cap_supported(caps, path_vision))
            m->caps |= MODEL_CAP_VISION;
         if (cap_supported(caps, path_pdf))
            m->caps |= MODEL_CAP_PDF;
         if (cap_supported(caps, path_thinking) || cap_supported(caps, path_adaptive))
            m->caps |= MODEL_CAP_REASONING;
         /* The specific shape, not just "can reason". This is the only source
          * that can tell adaptive from the retired budget_tokens form, and it is
          * what lets the request builder pick a config the model accepts instead
          * of one hardcoded years earlier. */
         if (cap_supported(caps, path_adaptive))
            m->caps |= MODEL_CAP_THINKING_ADAPTIVE;
      }
      /* Tool use is not a key in the published capability tree, so it cannot be
       * derived here; it stays clear and consumers must read 0 as "unknown".
       * Streaming likewise is not published — set per this codebase's standing
       * convention that every hosted model streams (model_registry.c's
       * heuristics ORs it in unconditionally), so a fetched row does not read as
       * "cannot stream" beside a declared one that does. */
      m->caps |= MODEL_CAP_STREAMING;
      count++;
   }

   cJSON_Delete(root);
   if (count == 0)
   {
      free(rows);
      return -1;
   }
   *models_out = rows;
   *n_out = count;
   return 0;
}

static const char *anthropic_default_headers[] = {"anthropic-version", "2023-06-01", NULL};

static const char *anthropic_routable_models[] = {"claude-opus-4-8", "claude-sonnet-5",
                                                  "claude-haiku-4-5", NULL};

model_provider_t anthropic_provider = {
    .name = "anthropic",
    .display_name = "Anthropic",
    .description = "Anthropic Claude API (claude-sonnet, claude-opus, claude-haiku)",
    .base_url = "https://api.anthropic.com/v1",
    .models_url = "https://api.anthropic.com/v1/models",
    .signup_url = "https://console.anthropic.com/account/keys",
    .auth_type = "x-api-key",
    .env_vars = anthropic_env_vars,
    .api_mode = API_MODE_ANTHROPIC_MESSAGES,
    .default_model = "claude-sonnet-4-6",
    .default_aux_model = "claude-haiku-4-5-20251001",
    .fallback_models = NULL,
    .routable_models = anthropic_routable_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = anthropic_default_headers,
    .fetch_models = anthropic_fetch_models,
};
