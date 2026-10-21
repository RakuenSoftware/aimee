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
static int cancel_status;
static const char *cancel_sequence = "9007199254740993";
int db1_call(db1_family_t *family, uint32_t op, const char *const *fields, uint32_t count,
             char *const *values, const size_t *caps, uint32_t slots, uint32_t *filled)
{
   (void)filled;
   assert(op == expected_op && family->timeout_ms == expected_budget);
   if (op == AIMEE_DB1_OP_SESSION_IR_CANCEL)
   {
      assert(count == 3 && !strcmp(fields[0], "actor") && !strcmp(fields[1], "session") &&
             !strcmp(fields[2], "exact-turn") && slots == 1 && caps[0] == 32);
      snprintf(values[0], caps[0], "%s", cancel_sequence);
      return cancel_status;
   }
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
   expected_budget = 30000;
   expected_op = AIMEE_DB1_OP_SESSION_IR_CANCEL;
   assert(!db1_session_ir_cancel("actor", "session", "exact-turn", sequence));
   assert(!strcmp(sequence, "9007199254740993"));
   cancel_sequence = "9223372036854775807";
   assert(!db1_session_ir_cancel("actor", "session", "exact-turn", sequence));
   const char *invalid[] = {"", "0", "01", "-1", "1x", "9223372036854775808"};
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      cancel_sequence = invalid[i];
      assert(db1_session_ir_cancel("actor", "session", "exact-turn", sequence) == -1);
      assert(!sequence[0]);
   }
   cancel_status = AIMEE_DB1_STATUS_INVALID;
   assert(db1_session_ir_cancel("actor", "session", "exact-turn", sequence) == 1 && !sequence[0]);
   cancel_status = AIMEE_DB1_STATUS_MISSING;
   assert(db1_session_ir_cancel("actor", "session", "exact-turn", sequence) == 1 && !sequence[0]);
   cancel_status = -1;
   assert(db1_session_ir_cancel("actor", "session", "exact-turn", sequence) == -1 && !sequence[0]);
   puts("DB1 bounded source deadlines: OK");
   return 0;
}
