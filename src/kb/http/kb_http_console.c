/* kb_http_console.c: /v1/console routes for the aimee-kb web console.
 * See kb_http_console.h. Reached only with a console-admin credential that the
 * route ACL (kb_route_acl.c) has already authorized. */
#include "kb_http_console.h"

#include <stdio.h>
#include <string.h>

/* Compare path to route, tolerating a single trailing slash (matching the route
 * ACL's normalization so the ACL and the handler agree on which path is which). */
static int route_is(const char *path, const char *route)
{
   size_t n = strlen(path);
   if (n > 1 && path[n - 1] == '/')
      n--;
   return strlen(route) == n && strncmp(path, route, n) == 0;
}

int kb_http_console_route(const char *method, const char *path, char *out_buf, int out_cap)
{
   if (!route_is(path, "/v1/console/overview"))
      return -1; /* not a console route — caller continues dispatch */

   if (strcmp(method, "GET") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }

   /* S0 stub. The real dashboard aggregate — an in-process fan-in over the kb
    * telemetry read models with per-source + global deadlines and per-component
    * {ok,degraded,data|error,fetched_at} — lands in S1. The envelope shape is
    * fixed now so the console has a real ACL'd route to gate against. */
   snprintf(out_buf, (size_t)out_cap,
            "{\"schema\":\"console.overview.v1\",\"components\":[],\"degraded\":false}");
   return 200;
}
