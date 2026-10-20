/* Real C wrapper routing: only source-scanning operations use the longer
 * bounded family. The generic transport retains caller deadline clamping. */
#include "db1_client/db1_module_api.h"
#include "db1_client/interaction_events.h"
#include "db1_client/primary_sessions.h"
#include "db1_client/transport.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t expected_op;
static uint64_t expected_budget;
static int calls;
int db1_call(db1_family_t *family, uint32_t op, const char *const *fields, uint32_t count,
             char *const *values, const size_t *caps, uint32_t slots, uint32_t *filled)
{
   (void)filled;
   assert(op == expected_op && family->timeout_ms == expected_budget);
   assert(count == 2 && !strcmp(fields[0], "actor") && !strcmp(fields[1], "{}"));
   calls++;
   if (slots)
   {
      assert(caps[0] >= 2);
      strcpy(values[0], "1");
   }
   if (slots > 1)
   {
      assert(caps[1] >= 2);
      strcpy(values[1], "0");
   }
   if (slots > 2)
   {
      assert(caps[2] >= 9);
      strcpy(values[2], "finished");
   }
   return AIMEE_DB1_STATUS_OK;
}
int main(void)
{
   char sequence[32], epoch[33];
   int absent = -1;
   expected_budget = 30000;
   expected_op = AIMEE_DB1_OP_SESSION_NATIVE_RESERVATION_RECONCILE;
   assert(!db1_session_native_reservation_reconcile("actor", "{}", sequence, epoch));
   expected_op = AIMEE_DB1_OP_SESSION_NATIVE_RESERVATION_FINISH;
   assert(!db1_session_native_reservation_finish_checked("actor", "{}", sequence, epoch, &absent));
   assert(!absent);
   expected_op = AIMEE_DB1_OP_DELEGATE_EXECUTION_EXPERIENCE_RECORD;
   assert(!db1_delegate_execution_experience_record("actor", "{}"));
   expected_budget = 2000;
   expected_op = AIMEE_DB1_OP_SESSION_NATIVE_EPOCH;
   assert(!db1_session_native_epoch("actor", "{}", epoch));
   assert(calls == 4);
   puts("DB1 bounded source deadlines: OK");
   return 0;
}
