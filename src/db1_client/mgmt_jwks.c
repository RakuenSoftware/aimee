/* mgmt_jwks.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "mgmt_jwks_cache.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static db1_family_t family = {"db1.mgmt_jwks", "mgmt jwks", AIMEE_DB1_EVENT_MGMT_JWKS, AIMEE_DB1_STAGE_MGMT_JWKS, 2000, 0};



int db1_mgmt_jwks_read(db1_mgmt_jwks_row_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char slot1[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->envelope, out->envelope_sha256, out->manifest_sha256, out->trust_bundle_sha256};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->envelope, sizeof out->envelope_sha256, sizeof out->manifest_sha256, sizeof out->trust_bundle_sha256};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MGMT_JWKS_READ, fields, 0, values, caps, 7, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   out->generation = (int64_t)strtoll(slot0, NULL, 10);
   out->valid_from = (int64_t)strtoll(slot1, NULL, 10);
   out->valid_until = (int64_t)strtoll(slot2, NULL, 10);
   return 1;
}

int db1_mgmt_jwks_generation(int64_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MGMT_JWKS_GENERATION, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   *out = (int64_t)strtoll(slot0, NULL, 10);
   return 1;
}

int db1_mgmt_jwks_install(const db1_mgmt_jwks_install_t *in)
{
   if (!in)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)in->valid_from);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)in->valid_until);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)in->fetched_at);
   const char *fields[] = {arg0, arg1, arg2, in->jwks, in->envelope, in->envelope_sha256, in->manifest_sha256, in->trust_bundle_sha256};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MGMT_JWKS_INSTALL, fields, 8, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

/* clang-format on */
