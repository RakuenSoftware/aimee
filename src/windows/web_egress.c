/* windows/web_egress.c -- see headers/web_egress.h.
 *
 * The pinned-connect primitive (agent_http_get_pinned) is implemented only for
 * posix, and posix is where the server runs. The Windows build is a thin client.
 *
 * Rather than silently doing something weaker here, the split is explicit:
 *
 *   WEB_EGRESS_CONFIGURED  -> REFUSED. This is the operator-configured search
 *                             endpoint, the destination that can be pointed at a
 *                             metadata address or an internal service. Without a
 *                             pinned connect there is no way to close the gap
 *                             between validating an address and connecting to
 *                             it, so it is not attempted.
 *
 *   WEB_EGRESS_UNTRUSTED   -> plain fetch. On this platform the only untrusted
 *                             callers are the two compile-time-constant search
 *                             endpoints, which are not attacker-selectable. This
 *                             is exactly the behaviour the thin client had
 *                             before this module existed, so nothing regresses.
 *
 * If the page reader is ever ported to Windows, or search gains page fetching
 * here, this file has to grow a real pinned implementation first. The refusal
 * above is what makes that a build-time conversation instead of a silent hole. */
#include "web_egress.h"

#include "agent_exec.h"

#include <stdlib.h>
#include <string.h>

int web_egress_addr_blocked(const struct sockaddr *sa)
{
   (void)sa;
   /* No resolution is performed on this platform, so nothing is classified. A
    * caller must not read this as "address is safe". */
   return 0;
}

int web_egress_private_endpoint_allowed(void)
{
   const char *v = getenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT");
   return (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
}

char *web_egress_fetch(const char *url, web_egress_policy_t policy, const char *extra_headers,
                       int timeout_ms, size_t max_bytes, const char **err)
{
   const char *local_err = NULL;
   if (!err)
      err = &local_err;
   *err = NULL;

   if (policy == WEB_EGRESS_CONFIGURED)
   {
      *err = "operator-configured endpoints are not fetched on this platform "
             "(no pinned-connect support; use the server build)";
      return NULL;
   }

   char *resp = NULL;
   int status = agent_http_get(url, extra_headers, &resp, timeout_ms);
   if (status < 0 || !resp)
   {
      free(resp);
      *err = "fetch failed";
      return NULL;
   }
   if (status >= 300 && status < 400)
   {
      free(resp);
      *err = "redirected; pass the final URL (redirects are not followed for egress safety)";
      return NULL;
   }
   if (status != 200)
   {
      free(resp);
      *err = "non-200 response";
      return NULL;
   }
   if (max_bytes && strlen(resp) > max_bytes)
      resp[max_bytes] = '\0';
   return resp;
}
