/* Native ABI adapter for the Go providers module's model registry. */
#include "model_registry.h"
#include "providers_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *string(cJSON *o, const char *k)
{
   const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
   return v ? v : "";
}
static double number(cJSON *o, const char *k)
{
   cJSON *v = cJSON_GetObjectItem(o, k);
   return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
static cJSON *call(const char *op, const char *provider, const char *model, const char *name)
{
   cJSON *args = cJSON_CreateObject();
   if (provider)
      cJSON_AddStringToObject(args, "provider", provider);
   if (model)
      cJSON_AddStringToObject(args, "model", model);
   if (name)
      cJSON_AddStringToObject(args, "name", name);
   cJSON *reply = providers_module_request(op, args, "server", 0);
   cJSON_Delete(args);
   return reply;
}
int providers_decode_capability(cJSON *v, model_capability_t *out)
{
   if (!cJSON_IsObject(v) || !out)
      return 0;
   memset(out, 0, sizeof(*out));
#define S(field, key) snprintf(out->field, sizeof(out->field), "%s", string(v, key))
   S(provider, "provider");
   S(model_id, "model");
   S(display_name, "display_name");
   S(modalities, "modalities");
   S(knowledge_cutoff, "knowledge_cutoff");
#undef S
#define N(field, key) out->field = number(v, key)
   N(context_window, "context_window");
   N(max_output, "max_output");
   N(cost_in_per_mtok, "cost_in_per_mtok");
   N(cost_out_per_mtok, "cost_out_per_mtok");
   N(cost_cache_read_per_mtok, "cost_cache_read_per_mtok");
   N(flags, "flags_mask");
#undef N
   out->open_weights = cJSON_IsTrue(cJSON_GetObjectItem(v, "open_weights"));
   out->deprecated = cJSON_IsTrue(cJSON_GetObjectItem(v, "deprecated"));
   cJSON *bands = cJSON_GetObjectItem(v, "price_bands");
   int n = cJSON_GetArraySize(bands);
   out->price_bands_truncated = n > MODEL_PRICE_BANDS_MAX;
   out->price_band_count = n > MODEL_PRICE_BANDS_MAX ? MODEL_PRICE_BANDS_MAX : n;
   for (int i = 0; i < out->price_band_count; i++)
   {
      cJSON *b = cJSON_GetArrayItem(bands, i);
      out->price_bands[i].above_tokens = number(b, "above_tokens");
      out->price_bands[i].in_per_mtok = number(b, "in_per_mtok");
      out->price_bands[i].out_per_mtok = number(b, "out_per_mtok");
      out->price_bands[i].cache_read_per_mtok = number(b, "cache_read_per_mtok");
   }
   return 1;
}
int model_alias_resolve(const char *alias, model_info_t *out)
{
   cJSON *r = call("metadata.alias", NULL, NULL, alias);
   cJSON *v = cJSON_GetObjectItem(r, "model");
   int ok = out && cJSON_IsObject(v);
   if (ok)
   {
      snprintf(out->provider, sizeof(out->provider), "%s", string(v, "provider"));
      snprintf(out->model_id, sizeof(out->model_id), "%s", string(v, "model"));
   }
   cJSON_Delete(r);
   return ok;
}
const char *model_detect_provider(const char *id)
{
   static _Thread_local char provider[MODEL_PROVIDER_MAX];
   cJSON *r = call("metadata.detect", NULL, id, NULL);
   snprintf(provider, sizeof(provider), "%s", string(r, "provider"));
   cJSON_Delete(r);
   return provider[0] ? provider : NULL;
}
int model_alias_list(model_info_t *out, int max)
{
   cJSON *r = call("metadata.aliases", NULL, NULL, NULL);
   cJSON *a = cJSON_GetObjectItem(r, "models");
   int n = number(r, "count");
   for (int i = 0; out && i < max && i < n; i++)
   {
      cJSON *v = cJSON_GetArrayItem(a, i);
      snprintf(out[i].provider, sizeof(out[i].provider), "%s", string(v, "provider"));
      snprintf(out[i].model_id, sizeof(out[i].model_id), "%s", string(v, "model"));
   }
   cJSON_Delete(r);
   return n;
}
int model_capability_get(const char *provider, const char *id, model_capability_t *out)
{
   cJSON *r = call("metadata.show", provider, id, NULL);
   int ok = providers_decode_capability(cJSON_GetObjectItem(r, "model"), out);
   cJSON_Delete(r);
   return ok;
}
int model_context_window(const char *id)
{
   cJSON *r = call("metadata.context_window", NULL, id, NULL);
   int n = number(cJSON_GetObjectItem(r, "model"), "context_window");
   cJSON_Delete(r);
   return n;
}
int model_max_output(const char *provider, const char *id)
{
   cJSON *r = call("metadata.max_output", provider, id, NULL);
   model_capability_t cap;
   int ok = providers_decode_capability(cJSON_GetObjectItem(r, "model"), &cap);
   cJSON_Delete(r);
   return ok ? cap.max_output : 0;
}
int model_capability_resolve_ref(const char *ref, char *provider, size_t pn, char *id, size_t in,
                                 model_capability_t *out)
{
   cJSON *r = call("metadata.show", NULL, NULL, ref);
   model_capability_t cap;
   int ok = providers_decode_capability(cJSON_GetObjectItem(r, "model"), &cap);
   if (ok && ((provider && strlen(cap.provider) >= pn) || (id && strlen(cap.model_id) >= in)))
      ok = 0;
   if (ok)
   {
      if (provider && pn)
         snprintf(provider, pn, "%s", cap.provider);
      if (id && in)
         snprintf(id, in, "%s", cap.model_id);
      if (out)
         *out = cap;
   }
   cJSON_Delete(r);
   return ok;
}
int model_capability_list(model_capability_t *out, int max, unsigned required, int open)
{
   cJSON *a = cJSON_CreateObject();
   cJSON_AddNumberToObject(a, "limit", max);
   cJSON_AddNumberToObject(a, "required_flags", required);
   cJSON_AddBoolToObject(a, "open_weights_only", open);
   cJSON *r = providers_module_request("metadata.list", a, "server", 0);
   cJSON_Delete(a);
   int n = number(r, "count");
   cJSON *v = cJSON_GetObjectItem(r, "models");
   for (int i = 0; out && i < max && i < cJSON_GetArraySize(v); i++)
      providers_decode_capability(cJSON_GetArrayItem(v, i), &out[i]);
   cJSON_Delete(r);
   return n;
}
int model_capability_refresh(char *msg, size_t n)
{
   cJSON *r = call("metadata.refresh", NULL, NULL, NULL);
   if (msg && n)
      snprintf(msg, n, "%s", string(r, "message"));
   int count = number(r, "count");
   cJSON_Delete(r);
   return count;
}
/* Enum/string conversion is part of the native ABI, with no catalog policy. */
unsigned model_capability_flag_from_name(const char *name)
{
   static const char *names[] = {"reasoning", "tools",     "vision",           "pdf",
                                 "audio",     "streaming", "thinking_adaptive"};
   if (!name)
      return 0;
   for (int i = 0; i < 7; i++)
      if (!strcasecmp(name, names[i]))
         return 1u << i;
   if (!strcasecmp(name, "tool"))
      return MODEL_CAP_TOOLS;
   if (!strcasecmp(name, "image"))
      return MODEL_CAP_VISION;
   if (!strcasecmp(name, "stream"))
      return MODEL_CAP_STREAMING;
   return 0;
}
void model_capability_format_flags(unsigned flags, char *buf, size_t n)
{
   static const char *names[] = {"reasoning", "tools",     "vision",           "pdf",
                                 "audio",     "streaming", "thinking_adaptive"};
   if (!buf || !n)
      return;
   buf[0] = 0;
   for (int i = 0; i < 7; i++)
      if (flags & (1u << i))
      {
         size_t len = strlen(buf);
         if (len < n)
            snprintf(buf + len, n - len, "%s%s", len ? "," : "", names[i]);
      }
}
void model_capability_flags_string(unsigned flags, char *buf, size_t n)
{
   model_capability_format_flags(flags, buf, n);
}
