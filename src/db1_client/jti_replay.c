/* jti_replay.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "server_identity_jti.h"
#include "server_management_jti.h"

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


static db1_family_t family = {"db1.jti_replay", "jti replay", AIMEE_DB1_EVENT_JTI_REPLAY, AIMEE_DB1_STAGE_JTI_REPLAY, 2000, 0};



server_identity_jti_result_t db1_identity_jti_consume_row(const db1_identity_jti_consume_t *in)
{
   if (!in)
      return -1;
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%lld", (long long)in->token.team_id);
   char arg7[32];
   snprintf(arg7, sizeof arg7, "%lld", (long long)in->token.issued_at);
   char arg8[32];
   snprintf(arg8, sizeof arg8, "%lld", (long long)in->token.expires_at);
   char arg9[32];
   snprintf(arg9, sizeof arg9, "%lld", (long long)in->consumed_at);
   const char *fields[] = {in->token.jti ? in->token.jti : "", in->token.issuer ? in->token.issuer : "", in->token.kid ? in->token.kid : "", in->token.audience ? in->token.audience : "", in->token.subject ? in->token.subject : "", arg5, in->token.tier ? in->token.tier : "", arg7, arg8, arg9};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_IDENTITY_JTI_CONSUME, fields, 10, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (server_identity_jti_result_t)strtoll(slot0, NULL, 10);
}

server_management_jti_result_t db1_management_jti_consume_row(const db1_management_jti_consume_t *in)
{
   if (!in)
      return -1;
   char arg5[32];
   snprintf(arg5, sizeof arg5, "%lld", (long long)in->token.team_id);
   char arg12[32];
   snprintf(arg12, sizeof arg12, "%lld", (long long)in->token.issued_at);
   char arg13[32];
   snprintf(arg13, sizeof arg13, "%lld", (long long)in->token.expires_at);
   char arg14[32];
   snprintf(arg14, sizeof arg14, "%lld", (long long)in->consumed_at);
   const char *fields[] = {in->token.jti ? in->token.jti : "", in->token.issuer ? in->token.issuer : "", in->token.kid ? in->token.kid : "", in->token.audience ? in->token.audience : "", in->token.subject ? in->token.subject : "", arg5, in->token.capability ? in->token.capability : "", in->token.peer_issuer ? in->token.peer_issuer : "", in->token.peer_serial ? in->token.peer_serial : "", in->token.peer_fingerprint ? in->token.peer_fingerprint : "", in->token.request_sha256 ? in->token.request_sha256 : "", in->token.correlation_id ? in->token.correlation_id : "", arg12, arg13, arg14};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_MANAGEMENT_JTI_CONSUME, fields, 15, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (server_management_jti_result_t)strtoll(slot0, NULL, 10);
}

/* clang-format on */
