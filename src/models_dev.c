/* Metadata refresh compatibility transport; downloads and storage belong to Go. */
#include "models_dev.h"
#include "providers_client.h"
#include "cJSON.h"
#include <string.h>
int models_dev_refresh(void)
{
   cJSON *r = providers_module_request("metadata.download", NULL, "server", 0);
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "status"));
   int ok = s && !strcmp(s, "ok");
   cJSON_Delete(r);
   return ok ? 0 : -1;
}
int models_dev_capability_get(const char *p, const char *m, model_capability_t *out)
{
   return models_dev_cache_lookup(p, m, out);
}
