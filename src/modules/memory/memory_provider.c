/* memory_provider.c: runtime registry for pluggable memory providers.
 *
 * Part of the pluggable context engine surface described in
 * docs/proposals/pending/plugin-extension-surface-and-context-engine.md.
 *
 * Enforcement: bundled provider always accepted; at most one external provider.
 * A second external registration is rejected with LOG_WARN.
 */
#include "memory_provider.h"
#include "log.h"
#include <string.h>

static memory_provider_t g_bundled;
static int g_bundled_set = 0;

static memory_provider_t g_external;
static int g_external_set = 0;

int memory_provider_register(const memory_provider_t *p)
{
   if (!p || !p->name || !p->name[0])
   {
      LOG_WARN("memory_provider", "register: NULL or unnamed provider rejected");
      return -1;
   }

   if (p->is_bundled)
   {
      g_bundled = *p;
      g_bundled_set = 1;
      LOG_INFO("memory_provider", "bundled provider '%s' registered", p->name);
      return 0;
   }

   /* External provider — cap at one */
   if (g_external_set)
   {
      LOG_WARN("memory_provider",
               "external provider '%s' rejected: '%s' already registered "
               "(bundled + one external is the maximum)",
               p->name, g_external.name);
      return -1;
   }

   g_external = *p;
   g_external_set = 1;
   LOG_INFO("memory_provider", "external provider '%s' registered", p->name);
   return 0;
}

const memory_provider_t *memory_provider_get_active(void)
{
   if (g_external_set)
      return &g_external;
   if (g_bundled_set)
      return &g_bundled;
   return NULL;
}

const memory_provider_t *memory_provider_get_bundled(void)
{
   return g_bundled_set ? &g_bundled : NULL;
}

const memory_provider_t *memory_provider_get_external(void)
{
   return g_external_set ? &g_external : NULL;
}

void memory_provider_reset(void)
{
   memset(&g_bundled, 0, sizeof(g_bundled));
   g_bundled_set = 0;
   memset(&g_external, 0, sizeof(g_external));
   g_external_set = 0;
}
