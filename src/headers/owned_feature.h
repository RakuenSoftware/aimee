#ifndef AIMEE_OWNED_FEATURE_H
#define AIMEE_OWNED_FEATURE_H
/* Optional opaque module ownership. This header contains no data-field policy,
 * source interpretation, principal/grant construction or legacy-store lookup. */
#include "cJSON.h"
#include <aimee/ir/aimee_ir.h>
#include <stdlib.h>
#include <string.h>
extern int aimee_module_owned_feature_status_v1(const char *) __attribute__((weak));
extern char *aimee_module_owned_feature_management_query_v1(const char *, const char *,
                                                           const char *) __attribute__((weak));
/* Versioned compiled choke-point evidence, not request or data authority. */
#if defined(__GNUC__) && !defined(__clang__)
#define AIMEE_OWNED_GUARD_CONTRACT_V1(name) \
   __attribute__((used, retain, externally_visible)) const unsigned name = 1
#else
#define AIMEE_OWNED_GUARD_CONTRACT_V1(name) \
   __attribute__((used, retain)) const unsigned name = 1
#endif
/* 0 alone means no selected owner. Unknown/failed ownership is fail-closed. */
static inline int aimee_owned_feature_status(const char *kind)
{
   if (!aimee_module_owned_feature_status_v1)
      return 0;
   int status = aimee_module_owned_feature_status_v1(kind);
   return status == 0 ? 0 : status == 1 ? 1 : -1;
}
static inline cJSON *aimee_owned_feature_outcome(const char *status)
{
   cJSON *out = cJSON_CreateObject();
   if (!out || !cJSON_AddStringToObject(out, "status", status))
   {
      cJSON_Delete(out);
      return NULL;
   }
   return out;
}
static inline cJSON *aimee_owned_feature_management_json(const char *kind, const char *operation,
                                                         const char *stable_request)
{
   if (aimee_owned_feature_status(kind) != 1 ||
       !aimee_module_owned_feature_management_query_v1)
      return aimee_owned_feature_outcome("unavailable");
   char *wire = aimee_module_owned_feature_management_query_v1(kind, operation, stable_request);
   cJSON *out = wire && strlen(wire) <= 900u * 1024u
                    ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
   free(wire);
   if (!cJSON_IsObject(out))
   {
      cJSON_Delete(out);
      return aimee_owned_feature_outcome("unavailable");
   }
   return out;
}
#endif
