#include "kb_mgmt_client.h"
#include "kb_mgmt_endpoint.h"
#include "kb_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kb_mgmt_client_request_auth(const char *ep, const char *ca, const char *cc, const char *ck,
                                const char *m, const char *path, const char *b, const char *auth,
                                char *r, size_t n, int *st)
{
   if (kb_mgmt_endpoint_validate(ep) != 0 || !ca || !m || !path || !r || !n)
      return -1;
   const char *p = ep + 8;
   char host[512];
   snprintf(host, sizeof(host), "%s", p);
   char *slash = strchr(host, '/');
   if (slash)
      *slash = 0;
   int port = 443;
   char *colon = strrchr(host, ':');
   if (colon && strchr(colon + 1, ']') == NULL)
   {
      *colon = 0;
      port = atoi(colon + 1);
      if (port <= 0 || port > 65535)
         return -1;
   }
   return kb_tls_client_request_auth(host, port, ca, cc, ck, m, path, b, auth, r, n, st);
}

int kb_mgmt_client_request(const char *ep, const char *ca, const char *cc, const char *ck,
                           const char *m, const char *path, const char *b, char *r, size_t n,
                           int *st)
{
   return kb_mgmt_client_request_auth(ep, ca, cc, ck, m, path, b, NULL, r, n, st);
}
