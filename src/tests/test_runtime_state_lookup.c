#include "../db1_client/runtime.c"
#include <assert.h>
static int reply_status;
static const char *reply_value, *reply_code;
int db1_call(db1_family_t *owner, uint32_t op, const char *const *fields, uint32_t count,
             char *const *values, const size_t *caps, uint32_t slots, uint32_t *filled)
{
   (void)filled;
   assert(owner->event_kind == AIMEE_DB1_EVENT_RUNTIME &&
          owner->stage_id == AIMEE_DB1_STAGE_RUNTIME);
   assert(op == AIMEE_DB1_OP_RUNTIME_STATE_GET && count == 1 && !strcmp(fields[0], "boot-owner"));
   assert(slots == 2);
   snprintf(values[0], caps[0], "%s", reply_value);
   snprintf(values[1], caps[1], "%s", reply_code);
   return reply_status;
}
int main(void)
{
   char out[64];
   reply_status = AIMEE_DB1_STATUS_OK;
   reply_value = "previous-owner";
   reply_code = "0";
   assert(!db1_runtime_state_lookup("boot-owner", out, sizeof out) && !strcmp(out, reply_value));
   reply_value = "";
   assert(!db1_runtime_state_lookup("boot-owner", out, sizeof out) && !out[0]);
   reply_status = AIMEE_DB1_STATUS_MISSING;
   reply_value = "stale-value";
   assert(db1_runtime_state_lookup("boot-owner", out, sizeof out) == 1 && !out[0]);
   assert(db1_runtime_state_get("boot-owner", out, sizeof out) == -1);
   const int failures[] = {-1, AIMEE_DB1_STATUS_FAILED, AIMEE_DB1_STATUS_INVALID};
   for (unsigned i = 0; i < sizeof failures / sizeof failures[0]; i++)
   {
      reply_status = failures[i];
      assert(db1_runtime_state_lookup("boot-owner", out, sizeof out) == -1 && !out[0]);
   }
   reply_status = AIMEE_DB1_STATUS_OK;
   const char *bad[] = {"", "-1", "1", "garbage", "0junk"};
   for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
   {
      reply_code = bad[i];
      assert(db1_runtime_state_lookup("boot-owner", out, sizeof out) == -1 && !out[0]);
   }
   assert(db1_runtime_state_lookup("", out, sizeof out) == -1 && !out[0]);
   assert(db1_runtime_state_lookup("boot-owner", NULL, sizeof out) == -1);
   puts("runtime state: typed missing is distinct from transport/store/malformed failures");
   return 0;
}
