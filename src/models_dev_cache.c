/* ABI compatibility for the single Go-owned model catalog. */
#include "models_dev.h"
#include "providers_client.h"
#include "cJSON.h"
extern int providers_decode_capability(cJSON *, model_capability_t *);
static int lookup(const char *op, const char *provider, const char *model, model_capability_t *out)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "provider", provider ? provider : "");
   cJSON_AddStringToObject(args, "model", model ? model : "");
   cJSON *reply = providers_module_request(op, args, "server", 0);
   cJSON_Delete(args);
   int ok = providers_decode_capability(cJSON_GetObjectItem(reply, "model"), out);
   cJSON_Delete(reply);
   return ok;
}
int models_dev_override_lookup(const char *p, const char *m, model_capability_t *out)
{
   return lookup("metadata.override", p, m, out);
}
int models_dev_cache_lookup(const char *p, const char *m, model_capability_t *out)
{
   return lookup("metadata.published", p, m, out);
}
int models_dev_cache_list(model_capability_t *out, int max, unsigned required, int open)
{
   return model_capability_list(out, max, required, open);
}
