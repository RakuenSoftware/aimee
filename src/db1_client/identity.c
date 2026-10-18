/* identity.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "db1_client/remote_client_grant.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static db1_family_t family = {"db1.identity", "identity", AIMEE_DB1_EVENT_IDENTITY, AIMEE_DB1_STAGE_IDENTITY, 2000, ATOMIC_VAR_INIT(0)};


/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_remote_client_claim_row(const char *principal, const char *new_bearer_sha256, int64_t now, db1_remote_client_claim_row_t *out)
{
   if (!principal || !principal[0] || !new_bearer_sha256 || !new_bearer_sha256[0] || !out)
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)now);
   const char *fields[] = {principal, new_bearer_sha256, arg2};
   char slot0[32];
   char slot4[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->grant.principal, out->grant.bearer_sha256, out->grant.cert_serial, slot4};
   const size_t caps[] = {sizeof slot0, sizeof out->grant.principal, sizeof out->grant.bearer_sha256, sizeof out->grant.cert_serial, sizeof slot4};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_REMOTE_CLIENT_CLAIM, fields, 3, values, caps, 5, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->result = (int)strtol(slot0, NULL, 10);
   out->grant.tier = (int)strtol(slot4, NULL, 10);
   return 0;
}

int db1_remote_client_abandon(const char *bearer_sha256)
{
   if (!bearer_sha256 || !bearer_sha256[0])
      return -1;
   const char *fields[] = {bearer_sha256};
   return write_result(db1_call(&family, AIMEE_DB1_OP_REMOTE_CLIENT_ABANDON, fields, 1, NULL, NULL, 0, NULL));
}

int db1_remote_client_bind(const char *bearer_sha256, const char *cert_serial, int64_t now)
{
   if (!bearer_sha256 || !bearer_sha256[0] || !cert_serial || !cert_serial[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)now);
   const char *fields[] = {bearer_sha256, cert_serial, arg2};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_REMOTE_CLIENT_BIND, fields, 3, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_remote_client_tier(const char *cert_serial, char *principal, size_t principal_cap)
{
   if (!cert_serial || !cert_serial[0] || !principal || principal_cap == 0)
      return -1;
   const char *fields[] = {cert_serial};
   char slot_rc[32];
   char *const values[] = {principal, slot_rc};
   const size_t caps[] = {principal_cap, sizeof slot_rc};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_REMOTE_CLIENT_TIER, fields, 1, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

/* clang-format on */

int db1_remote_client_manage(const char *principal, const char *action, const char *id,
                             const char *name, int64_t now, char *out, size_t cap)
{
   if (!principal || !action || !id || !name || !out || !cap)
      return -1;
   char stamp[32];
   snprintf(stamp, sizeof stamp, "%lld", (long long)now);
   const char *fields[] = {principal, action, id, name, stamp};
   char *const values[] = {out};
   const size_t caps[] = {cap};
   return write_result(
       db1_call(&family, AIMEE_DB1_OP_REMOTE_CLIENT_MANAGE, fields, 5, values, caps, 1, NULL));
}
