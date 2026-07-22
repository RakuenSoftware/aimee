/* kb_oidc_jwks_fleet.c: Postgres-backed fleet JWKS source (P1 I10). See header. */

#include "kb_oidc_jwks_fleet.h"

#include "kb_auth_oidc.h" /* kb_oidc_set_fleet_resolver */
#include "oidc_jwks.h"    /* db2_jwks_list_active */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int kb_oidc_jwks_assemble(const char *const *jwk_objects, int n, char *out, size_t cap)
{
   if (!out || cap == 0 || n < 0)
      return -1;
   size_t o = 0;
#define APPEND(s)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      size_t _l = strlen(s);                                                                       \
      if (o + _l >= cap)                                                                           \
         return -1;                                                                                \
      memcpy(out + o, (s), _l);                                                                    \
      o += _l;                                                                                     \
   } while (0)
   APPEND("{\"keys\":[");
   for (int i = 0; i < n; ++i)
   {
      if (!jwk_objects[i])
         return -1;
      if (i)
         APPEND(",");
      APPEND(jwk_objects[i]);
   }
   APPEND("]}");
#undef APPEND
   out[o] = '\0';
   return 0;
}

/* Bounded-refresh cache: one entry per issuer (kb usually has a single configured
 * issuer). TTL default 300s — <= the 900s token-age ceiling (I9), so a retired IdP
 * key stops being trusted fleet-wide within the TTL. */
#define FLEET_TTL_SECS    300
#define FLEET_MAX_ISSUERS 8
#define FLEET_JWKS_MAX    16384

struct fleet_ent
{
   char issuer[256];
   char jwks[FLEET_JWKS_MAX];
   int have;
   time_t at;
};
static struct fleet_ent g_fleet[FLEET_MAX_ISSUERS];
static pthread_mutex_t g_fleet_lock = PTHREAD_MUTEX_INITIALIZER;

int kb_oidc_jwks_fleet_get(const char *issuer, char *out, size_t cap)
{
   if (!issuer || !issuer[0] || !out || cap == 0)
      return -1;
   time_t now = time(NULL);

   pthread_mutex_lock(&g_fleet_lock);
   struct fleet_ent *e = NULL;
   for (int i = 0; i < FLEET_MAX_ISSUERS; ++i)
      if (g_fleet[i].issuer[0] && strcmp(g_fleet[i].issuer, issuer) == 0)
      {
         e = &g_fleet[i];
         break;
      }
   if (e && now - e->at <= FLEET_TTL_SECS)
   {
      int rc = -1;
      if (e->have && strlen(e->jwks) < cap)
      {
         memcpy(out, e->jwks, strlen(e->jwks) + 1);
         rc = 0;
      }
      pthread_mutex_unlock(&g_fleet_lock);
      return rc;
   }
   pthread_mutex_unlock(&g_fleet_lock);

   /* Cache miss / stale: read the source of truth. */
   db2_jwks_row_t rows[32];
   int n = db2_jwks_list_active(issuer, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   char jwks[FLEET_JWKS_MAX];
   int have = 0;
   if (n > 0)
   {
      const char *objs[32];
      for (int i = 0; i < n; ++i)
         objs[i] = rows[i].jwk_json;
      if (kb_oidc_jwks_assemble(objs, n, jwks, sizeof(jwks)) == 0)
         have = 1;
   }

   pthread_mutex_lock(&g_fleet_lock);
   /* find-or-allocate an entry for this issuer */
   e = NULL;
   for (int i = 0; i < FLEET_MAX_ISSUERS; ++i)
      if (g_fleet[i].issuer[0] && strcmp(g_fleet[i].issuer, issuer) == 0)
      {
         e = &g_fleet[i];
         break;
      }
   if (!e)
      for (int i = 0; i < FLEET_MAX_ISSUERS; ++i)
         if (!g_fleet[i].issuer[0])
         {
            e = &g_fleet[i];
            snprintf(e->issuer, sizeof(e->issuer), "%s", issuer);
            break;
         }
   if (e)
   {
      e->have = have;
      e->at = now;
      if (have)
         snprintf(e->jwks, sizeof(e->jwks), "%s", jwks);
      else
         e->jwks[0] = '\0';
   }
   int rc = -1;
   if (have && strlen(jwks) < cap)
   {
      memcpy(out, jwks, strlen(jwks) + 1);
      rc = 0;
   }
   pthread_mutex_unlock(&g_fleet_lock);
   return rc;
}

void kb_oidc_jwks_fleet_enable(void)
{
   kb_oidc_set_fleet_resolver(kb_oidc_jwks_fleet_get);
}
