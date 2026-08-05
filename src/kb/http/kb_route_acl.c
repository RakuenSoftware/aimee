/* kb_route_acl.c: KB event-bus seam for control-web route authorization and
 * the separate core maintenance-family classifier. See kb_route_acl.h. */
#include "kb_route_acl.h"

#include <string.h>

static kb_route_authorization_provider_fn g_authorization_provider;

void kb_route_acl_register_authorization_provider(kb_route_authorization_provider_fn provider)
{
   g_authorization_provider = provider;
}

int kb_route_acl_console_admin_authorize(const char *method, const char *path, int *allowed)
{
   if (!allowed)
      return -1;
   *allowed = 0;
   return g_authorization_provider ? g_authorization_provider(method, path, allowed) : -1;
}

/* Prefix matching is deliberate for the destructive maintenance family: a
 * newly added maintenance route remains owner-only without updating a list.
 * This is core KB execution policy, not control-web proxy authorization. */
int kb_route_acl_is_maintenance(const char *path)
{
   return path && strncmp(path, "/v1/maintenance/", 16) == 0;
}
