/* kb_route_acl.c: static console-admin route allowlist. See kb_route_acl.h. */
#include "kb_route_acl.h"

#include <string.h>

/* One allowlist entry: an HTTP method + a route pattern. A "{id}" pattern
 * segment matches exactly one non-empty, slash-free path segment; all other
 * segments match literally. Routes that later slices add (enrollments in S2a,
 * config/oidc in S2b, decisions/audit in S4) are listed here up front so the
 * containment table is defined once, in S0; an entry for a not-yet-registered
 * route is harmless (the request still 404s in dispatch). */
struct acl_entry
{
   const char *method;
   const char *pattern;
};

static const struct acl_entry CONSOLE_ADMIN_ACL[] = {
    {"GET", "/v1/console/overview"},
    {"GET", "/v1/enrollments"},
    {"POST", "/v1/enrollments/{id}/revoke"},
    {"GET", "/v1/config/oidc"},
    {"PUT", "/v1/config/oidc"},
    {"GET", "/v1/scopes"},
    {"GET", "/v1/decisions"},
    {"GET", "/v1/decisions/{id}"},
    {"POST", "/v1/decisions"},
    {"POST", "/v1/decisions/{id}/supersede"},
    {"POST", "/v1/decisions/{id}/outcome"},
    {"POST", "/v1/decisions/{id}/status"},
    {"POST", "/v1/decisions/{id}/revisit"},
    {"GET", "/v1/audit/actions"},
};

/* Match one segment: "{id}" accepts any non-empty segment, else exact bytes. */
static int seg_matches(const char *pat, size_t plen, const char *seg, size_t slen)
{
   if (plen == 4 && memcmp(pat, "{id}", 4) == 0)
      return slen > 0; /* segments never contain '/' by construction */
   return plen == slen && memcmp(pat, seg, slen) == 0;
}

/* Segment-exact match of a route pattern against a path. Both begin with '/'. */
static int path_matches(const char *pattern, const char *path)
{
   const char *p = pattern, *q = path;
   for (;;)
   {
      if (*p != '/' || *q != '/')
         return 0; /* leading '/' guaranteed by the caller */
      p++;
      q++;
      const char *ps = p, *qs = q;
      while (*p && *p != '/')
         p++;
      while (*q && *q != '/')
         q++;
      if (!seg_matches(ps, (size_t)(p - ps), qs, (size_t)(q - qs)))
         return 0;
      int pend = (*p == '\0'), qend = (*q == '\0');
      if (pend && qend)
         return 1;
      if (pend != qend)
         return 0; /* unequal segment counts — no prefix/suffix widening */
   }
}

int kb_route_acl_console_admin_allows(const char *method, const char *path)
{
   if (!method || !path || path[0] != '/')
      return 0;

   /* Normalize a single trailing slash (but keep a bare "/"). Reject absurdly
    * long paths outright rather than truncating (truncation could alias). */
   char buf[512];
   size_t n = strlen(path);
   if (n >= sizeof(buf))
      return 0;
   if (n > 1 && path[n - 1] == '/')
      n--;
   memcpy(buf, path, n);
   buf[n] = '\0';

   for (size_t i = 0; i < sizeof(CONSOLE_ADMIN_ACL) / sizeof(CONSOLE_ADMIN_ACL[0]); i++)
   {
      if (strcmp(method, CONSOLE_ADMIN_ACL[i].method) == 0 &&
          path_matches(CONSOLE_ADMIN_ACL[i].pattern, buf))
         return 1;
   }
   return 0;
}
