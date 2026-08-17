#include "../modules/db2/support/db2_log.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
   int calls;
   log_level_t level;
   char module[32];
   char message[1024];
} captured_log_t;

static void capture(void *context, log_level_t level, const char *module, const char *message)
{
   captured_log_t *captured = context;
   captured->calls++;
   captured->level = level;
   snprintf(captured->module, sizeof(captured->module), "%s", module);
   snprintf(captured->message, sizeof(captured->message), "%s", message);
}

int main(void)
{
   captured_log_t captured = {0};

   /* Logging before process startup installs its sink is intentionally inert. */
   aimee_log(LOG_WARN, "db2", "not installed");

   db2_log_install(capture, &captured);
   aimee_log(LOG_INFO, "db2.test", "operation=%d state=%s", 17, "ready");
   if (captured.calls != 1 || captured.level != LOG_INFO ||
       strcmp(captured.module, "db2.test") != 0 ||
       strcmp(captured.message, "operation=17 state=ready") != 0)
      return 1;

   char oversized[2048];
   memset(oversized, 'x', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = '\0';
   aimee_log(LOG_ERROR, "db2.long", "%s", oversized);
   if (captured.calls != 2 || strlen(captured.message) != sizeof(captured.message) - 1)
      return 2;

   aimee_log((log_level_t)-1, "db2", "invalid");
   aimee_log(LOG_WARN, NULL, "invalid");
   aimee_log(LOG_WARN, "db2", NULL);
   if (captured.calls != 2)
      return 3;

   db2_log_install(NULL, NULL);
   aimee_log(LOG_ERROR, "db2", "disabled");
   return captured.calls == 2 ? 0 : 4;
}
