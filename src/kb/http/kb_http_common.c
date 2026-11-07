/* Shared KB HTTP framing and authenticated tenant-scope entry. */
#include "kb_http_common.h"
#include "cJSON.h"
#include "kb_reqctx.h"
#include "modules/kb/c/kb_store_tenant.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kb_http_error(char *out, int cap, int status, const char *msg)
{
   snprintf(out, (size_t)cap, "{\"error\":\"%s\"}", msg);
   return status;
}

int kb_http_emit(cJSON *root, char *out, int cap, int status)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s || (int)strlen(s) >= cap)
   {
      free(s);
      snprintf(out, (size_t)cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   snprintf(out, (size_t)cap, "%s", s);
   free(s);
   return status;
}

int kb_http_tenant_status(int rc)
{
   if (rc == KB_STORE_ERR_TENANT_REQUIRES_PG)
      return 503;
   if (rc == KB_STORE_ERR_TENANT_UNAUTHENTICATED)
      return 401;
   if (rc == KB_STORE_ERR_TENANT_DENIED)
      return 403;
   return 500;
}

int kb_http_query_param(const char *qs, const char *key, char *out, size_t cap)
{
   if (cap == 0)
      return 0;
   out[0] = '\0';
   if (!qs)
      return 0;
   size_t klen = strlen(key);
   const char *p = qs;
   while (p && *p)
   {
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         const char *v = p + klen + 1;
         const char *amp = strchr(v, '&');
         size_t n = amp ? (size_t)(amp - v) : strlen(v);
         if (n >= cap)
            n = cap - 1;
         memcpy(out, v, n);
         out[n] = '\0';
         return 1;
      }
      p = strchr(p, '&');
      if (p)
         p++;
   }
   return 0;
}

int kb_http_begin_actor_scope(char *out, int cap, int *http_out)
{
   const kb_principal_t *actor = kb_reqctx_actor();
   if (!actor)
   {
      *http_out = kb_http_error(out, cap, 401, "authentication required");
      return -1;
   }
   int rc = kb_store_tenant_scope_begin(actor, 0);
   if (rc != 0)
   {
      *http_out = kb_http_error(out, cap, kb_http_tenant_status(rc), "tenant scope failed");
      return -1;
   }
   return 0;
}

int kb_http_parse_positive_int64(const char *s, int64_t *out)
{
   if (!s || !s[0])
      return 0;
   int64_t v = 0;
   for (const char *p = s; *p; ++p)
   {
      if (*p < '0' || *p > '9')
         return 0;
      if (v > (INT64_MAX - (*p - '0')) / 10)
         return 0;
      v = v * 10 + (*p - '0');
   }
   if (v <= 0)
      return 0;
   *out = v;
   return 1;
}
