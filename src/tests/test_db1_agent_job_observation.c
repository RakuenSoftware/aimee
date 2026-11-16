#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include "db1_client/db1_module_api.h"
#include "db1_client/agent_jobs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int available = 1, calls, mode;
static char value[7001];
static uint64_t first_deadline;
int obs_bus_module_available(uint32_t event)
{
   assert(event == AIMEE_DB1_EVENT_DELEGATION);
   return available;
}
ouro_module_call_result_t obs_bus_module_call(uint32_t event, uint32_t stage, uint64_t trace,
                                              uint64_t deadline, const void *request,
                                              uint32_t request_len, void *response,
                                              uint32_t capacity, uint32_t *length,
                                              ouro_module_cancelled_fn cancel, void *context)
{
   (void)trace;
   (void)cancel;
   (void)context;
   assert(event == AIMEE_DB1_EVENT_DELEGATION && stage == AIMEE_DB1_STAGE_DELEGATION && deadline);
   assert(request_len == 23);
   const uint8_t *r = request;
   assert(aimee_db1_get_u32(r) == AIMEE_DB1_OP_AGENT_JOB_CANONICAL_OBSERVATION &&
          aimee_db1_get_u32(r + 4) == 2);
   assert(aimee_db1_get_u32(r + 8) == 2 && !memcmp(r + 12, "91", 2));
   assert(aimee_db1_get_u32(r + 14) == 5 && !memcmp(r + 18, "owner", 5));
   if (!calls)
      first_deadline = deadline;
   else
      assert(deadline == first_deadline);
   calls++;
   if (mode == 1)
      return OURO_MODULE_CALL_TRANSPORT;
   if (mode == 5)
   {
      *length = OURO_MODULE_MESSAGE_MAX_BODY + 1;
      return OURO_MODULE_CALL_RESPONSE_TOO_LARGE;
   }
   uint8_t *out = response;
   if (mode == 2 || mode == 7)
   {
      aimee_db1_put_u32(out, mode == 7 ? AIMEE_DB1_STATUS_INVALID : AIMEE_DB1_STATUS_MISSING);
      aimee_db1_put_u32(out + 4, 0);
      *length = 8;
      return OURO_MODULE_CALL_OK;
   }
   if (mode == 8)
   {
      aimee_db1_put_u32(out, AIMEE_DB1_STATUS_OK);
      aimee_db1_put_u32(out + 4, 1);
      aimee_db1_put_u32(out + 8, 0);
      *length = 12;
      return OURO_MODULE_CALL_OK;
   }
   *length = 12 + sizeof(value) - 1;
   if (capacity < *length)
      return OURO_MODULE_CALL_RESPONSE_TOO_LARGE;
   aimee_db1_put_u32(out, mode == 6 ? AIMEE_DB1_STATUS_INVALID : AIMEE_DB1_STATUS_OK);
   aimee_db1_put_u32(out + 4, 1);
   aimee_db1_put_u32(out + 8, sizeof(value) - 1);
   memcpy(out + 12, value, sizeof(value) - 1);
   if (mode == 3)
      aimee_db1_put_u32(out + 8, sizeof(value));
   if (mode == 4)
      out[25] = 0;
   return OURO_MODULE_CALL_OK;
}
int main(void)
{
   memset(value, 'x', sizeof(value) - 1);
   char *out = (char *)1;
   assert(db1_agent_job_canonical_observation(0, "owner", &out) == -1 && !out);
   assert(db1_agent_job_canonical_observation(91, "", &out) == -1 && !out);
   assert(db1_agent_job_canonical_observation(91, "owner", NULL) == -1);
   available = 0;
   assert(db1_agent_job_canonical_observation(91, "owner", &out) == -1 && !out && calls == 0);
   available = 1;
   assert(db1_agent_job_canonical_observation(91, "owner", &out) == 0 && out &&
          !strcmp(out, value) && calls == 2);
   free(out);
   for (mode = 1; mode <= 8; mode++)
   {
      calls = 0;
      out = (char *)1;
      int rc = db1_agent_job_canonical_observation(91, "owner", &out);
      assert(rc == (mode == 2 ? 1 : mode == 7 ? -2 : -1) && !out);
      assert(calls == (mode == 3 || mode == 4 || mode == 6 ? 2 : 1));
   }
   puts("agent-job observation client: exact actor/job framing, bounded dynamic reply, "
        "missing/error separation");
   return 0;
}
