/* Immutable native ABI views of Go-owned provider profiles. */
#include "model_provider.h"
#include "providers_client.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static model_provider_t *profiles;
static int count;
static const char *text(cJSON *row, const char *key)
{
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(row, key));
   return s ? s : "";
}
static const char **strings(cJSON *row, const char *key)
{
   cJSON *a = cJSON_GetObjectItem(row, key);
   int n = cJSON_GetArraySize(a);
   const char **out = calloc(n + 1, sizeof(*out));
   if (!out)
      return NULL;
   for (int i = 0; i < n; i++)
      out[i] = strdup(cJSON_GetStringValue(cJSON_GetArrayItem(a, i)));
   return out;
}
static int fetch(model_provider_t *p, provider_model_t **out, int *n)
{
   *out = NULL;
   *n = 0;
   cJSON *a = cJSON_CreateObject();
   cJSON_AddStringToObject(a, "name", p->name);
   cJSON *r = providers_module_request("provider.models", a, "server", 0);
   cJSON_Delete(a);
   if (strcmp(text(r, "status"), "ok"))
   {
      cJSON_Delete(r);
      return -1;
   }
   cJSON *models = cJSON_GetObjectItem(r, "details");
   int count = cJSON_GetArraySize(models);
   provider_model_t *v = calloc(count ? count : 1, sizeof(*v));
   if (!v)
   {
      cJSON_Delete(r);
      return -1;
   }
   for (int i = 0; i < count; i++)
   {
      cJSON *m = cJSON_GetArrayItem(models, i);
      snprintf(v[i].id, sizeof(v[i].id), "%s", text(m, "id"));
      snprintf(v[i].display_name, sizeof(v[i].display_name), "%s", text(m, "display_name"));
      cJSON *ctx = cJSON_GetObjectItem(m, "context_window"),
            *max = cJSON_GetObjectItem(m, "max_output");
      v[i].context_window = cJSON_IsNumber(ctx) ? ctx->valueint : 0;
      v[i].max_output = cJSON_IsNumber(max) ? max->valueint : 0;
      v[i].deprecated = cJSON_IsTrue(cJSON_GetObjectItem(m, "deprecated"));
   }
   cJSON_Delete(r);
   *out = v;
   *n = count;
   return 0;
}
static int classify(model_provider_t *p, int status, const char *body, failover_reason_t *out)
{
   cJSON *a = cJSON_CreateObject();
   cJSON_AddStringToObject(a, "name", p->name);
   cJSON_AddNumberToObject(a, "http_status", status);
   cJSON_AddStringToObject(a, "body", body ? body : "");
   cJSON *r = providers_module_request("provider.classify", a, "server", 0);
   cJSON_Delete(a);
   cJSON *reason = cJSON_GetObjectItem(r, "reason");
   int v = cJSON_IsNumber(reason) ? reason->valueint : 0;
   cJSON_Delete(r);
   if (v && out)
      *out = v;
   return v != 0;
}
static void load(void)
{
   if (profiles)
      return;
   cJSON *r = providers_module_request("provider.profiles", NULL, "server", 0);
   cJSON *a = cJSON_GetObjectItem(r, "providers");
   int n = cJSON_GetArraySize(a);
   if (n <= 0)
   {
      cJSON_Delete(r);
      return;
   }
   model_provider_t *values = calloc(n, sizeof(*values));
   if (!values)
   {
      cJSON_Delete(r);
      return;
   }
   for (int i = 0; i < n; i++)
   {
      model_provider_t *p = &values[i];
      cJSON *v = cJSON_GetArrayItem(a, i);
#define S(k) p->k = strdup(text(v, #k))
      S(name);
      S(display_name);
      S(description);
      S(base_url);
      S(models_url);
      S(signup_url);
      S(auth_type);
      S(default_model);
      S(default_aux_model);
#undef S
#define A(k) p->k = strings(v, #k)
      A(env_vars);
      A(fallback_models);
      A(routable_models);
      A(default_headers);
#undef A
#define N(k)                                                                                       \
   {                                                                                               \
      cJSON *j = cJSON_GetObjectItem(v, #k);                                                       \
      p->k = cJSON_IsNumber(j) ? j->valueint : 0;                                                  \
   }
      N(api_mode);
      N(fixed_temperature);
      N(default_max_tokens);
#undef N
      p->fetch_models = fetch;
      p->classify_body = classify;
   }
   cJSON_Delete(r);
   profiles = values;
   count = n;
}
model_provider_t *model_provider_get(const char *name)
{
   if (!name)
      return NULL;
   pthread_mutex_lock(&lock);
   load();
   model_provider_t *p = NULL;
   for (int i = 0; i < count; i++)
      if (!strcmp(name, profiles[i].name))
      {
         p = &profiles[i];
         break;
      }
   pthread_mutex_unlock(&lock);
   return p;
}
int model_provider_list(model_provider_t **out, int max)
{
   pthread_mutex_lock(&lock);
   load();
   int n = count < max ? count : max;
   for (int i = 0; out && i < n; i++)
      out[i] = &profiles[i];
   pthread_mutex_unlock(&lock);
   return n;
}
