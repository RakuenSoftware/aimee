/* mgmt_nonce.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "mgmt_nonce.h"

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


static db1_family_t family = {"db1.mgmt_nonce", "mgmt nonce", AIMEE_DB1_EVENT_MGMT_NONCE, AIMEE_DB1_STAGE_MGMT_NONCE, 2000, 0};


/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_mgmt_nonce_clear()
{
   const char *const *fields = NULL;
   return write_result(db1_call(&family, AIMEE_DB1_OP_MGMT_NONCE_CLEAR, fields, 0, NULL, NULL, 0, NULL));
}

int db1_mgmt_nonce_issue(const db1_mgmt_nonce_issue_t *in)
{
   if (!in)
      return -1;
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%lld", (long long)in->now);
   const char *fields[] = {in->nonce, in->peer_issuer, in->peer_serial_norm, in->peer_fingerprint, in->channel_binding, in->target_server_id, in->purpose, arg7};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MGMT_NONCE_ISSUE, fields, 8, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_mgmt_nonce_consume(const db1_mgmt_nonce_consume_t *in)
{
   if (!in)
      return -1;
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%lld", (long long)in->now);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%lld", (long long)in->revocation_generation);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%d", in->valid);
   const char *fields[] = {in->nonce, in->peer_issuer, in->peer_serial_norm, in->peer_fingerprint, in->channel_binding, in->target_server_id, in->purpose, arg7, arg8, arg9};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MGMT_NONCE_CONSUME, fields, 10, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_mgmt_status_hwm_read(int64_t *generation)
{
   if (!generation)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MGMT_STATUS_HWM_READ, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *generation = (int64_t)strtoll(slot0, NULL, 10);
   return 0;
}

int db1_mgmt_status_hwm_set(int64_t generation)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)generation);
   const char *fields[] = {arg0};
   return write_result(db1_call(&family, AIMEE_DB1_OP_MGMT_STATUS_HWM_SET, fields, 1, NULL, NULL, 0, NULL));
}

/* clang-format on */
