/* test_db1_agent_job_list.c: the recent-jobs list must fit one ring message.
 *
 * Every reply slot's capacity counts toward a single module message, capped at
 * OURO_MODULE_MESSAGE_MAX_BODY. The list reserved 1 MiB for each row's prompt and
 * result even for the light listing (which returns both empty), so 8 rows asked
 * for 16 MiB and the call was refused before it was sent: `aimee jobs list`
 * answered "could not read agent jobs" on any Server with 8+ delegate jobs. */
#include "db1_client/agent_jobs.h"
#include "db1_module_api.h"
#include "support/module_bus_stub.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A light-listing reply of `rows` rows, 16 cells each, prompt/result empty. */
static uint8_t *light_reply(int rows, uint32_t *len_out)
{
   size_t cap = 8u + (size_t)rows * 16u * (4u + 32u);
   uint8_t *buf = calloc(1, cap);
   assert(buf);
   aimee_db1_put_u32(buf, AIMEE_DB1_STATUS_OK);
   aimee_db1_put_u32(buf + 4, (uint32_t)rows * 16u);
   size_t at = 8;
   for (int r = 0; r < rows; r++)
   {
      char id[16];
      snprintf(id, sizeof id, "%d", 100 - r);
      const char *cells[16] = {id,
                               "summarize",
                               "",
                               "codex",
                               "p",
                               "failed",
                               "",
                               "0",
                               "925",
                               "",
                               "",
                               "0",
                               "0",
                               "0",
                               "2026-11-19 08:00:00",
                               "2026-11-19 08:00:00"};
      for (int c = 0; c < 16; c++)
      {
         uint32_t n = (uint32_t)strlen(cells[c]);
         assert(at + 4 + n <= cap);
         aimee_db1_put_u32(buf + at, n);
         memcpy(buf + at + 4, cells[c], n);
         at += 4 + n;
      }
   }
   *len_out = (uint32_t)at;
   return buf;
}

int main(void)
{
   db1_agent_job_t jobs[64];

   /* The live failure: 20 rows (the CLI default) through the light listing. */
   uint32_t len = 0;
   uint8_t *reply = light_reply(20, &len);
   module_bus_stub_reply_bytes(reply, len);
   int n = db1_agent_job_list_recent(jobs, 20, 0);
   assert(n == 20);
   assert(jobs[0].id == 100 && jobs[19].id == 81);
   assert(!strcmp(jobs[0].role, "summarize") && !strcmp(jobs[0].status, "failed"));
   for (int i = 0; i < n; i++)
   {
      free(jobs[i].prompt);
      free(jobs[i].result);
   }

   /* The heavy listing still carries full bodies; it is clamped to what one
    * message can hold instead of failing the whole call. */
   free(reply);
   reply = light_reply(3, &len);
   module_bus_stub_reply_bytes(reply, len);
   n = db1_agent_job_list_recent(jobs, 64, 1);
   assert(n == 3);
   for (int i = 0; i < n; i++)
   {
      free(jobs[i].prompt);
      free(jobs[i].result);
   }
   free(reply);
   puts("db1_agent_job_list: light list fits one message; heavy list is clamped");
   return 0;
}
