/* Native views consume complete Go envelopes through named host discovery. */
#include "cJSON.h"
#include "json_fluent.h"
#include "session_briefing.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *api_memory_stats(void);
extern char *api_dashboard_reminders(void);
extern char *api_dashboard_directives(void);
static const char *expected_operation;
static const char *reply;
static int expected_limit, transport_result = 1, initialized = 1, calls;

int kb_store_is_initialized(void)
{
   return initialized;
}
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   assert(timeout_ms == 60000);
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(jo_cstr(args, "operation"), expected_operation) == 0);
   if (strstr(expected_operation, "briefing"))
      assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(args, "limit")) ==
             expected_limit);
   calls++;
   *result = reply ? cJSON_Parse(reply) : NULL;
   return transport_result;
}
int main(void)
{
   char *value = NULL;
   struct view
   {
      const char *operation;
      char *(*render)(void);
   } views[] = {
       {"stats-dashboard", api_memory_stats},
       {"prospective-dashboard", api_dashboard_reminders},
       {"directive-dashboard", api_dashboard_directives},
   };
   for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++)
   {
      expected_operation = views[i].operation;
      reply = "{\"status\":\"ok\",\"dashboard\":{\"count\":4}}";
      value = views[i].render();
      assert(value && strcmp(value, "{\"count\":4}") == 0);
      free(value);
      const char *failures[] = {NULL, "{}", "[]", "{\"status\":\"error\",\"dashboard\":{}}"};
      for (size_t j = 0; j < sizeof(failures) / sizeof(failures[0]); j++)
      {
         reply = failures[j];
         assert(views[i].render() == NULL);
      }
      reply = "{\"status\":\"ok\",\"dashboard\":{}}";
      transport_result = -1;
      assert(views[i].render() == NULL);
      transport_result = 1;
   }
   initialized = 0;
   int before = calls;
   assert(api_dashboard_reminders() == NULL && api_dashboard_directives() == NULL);
   assert(calls == before);
   puts("native memory views generic transport: ok");
   return 0;
}
