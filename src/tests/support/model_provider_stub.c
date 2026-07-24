/* model_provider_stub.c: stub the provider registry lookup for unit tests that
 * link agent_config.o but don't exercise provider-general expansion.
 *
 * agent_load_config consults model_provider_get() to discover a registered
 * provider's routable models (registering "codex" expands to codex:sol,
 * codex:terra, codex:luna). The real registry (server/model_provider.c) pulls
 * in every provider profile, and each profile's fetch_models path pulls in
 * agent_http_get — a whole HTTP stack a config unit test shouldn't need.
 *
 * "no such provider registered" is the correct test default: expansion is then
 * skipped and the agent loads as the single explicit target it declares. */
#include "model_provider.h"
#include <stddef.h>

model_provider_t *model_provider_get(const char *name)
{
   (void)name;
   return NULL;
}
