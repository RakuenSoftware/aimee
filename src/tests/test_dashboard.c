/* test_dashboard.c: dashboard plugin and audit log API coverage */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include "aimee.h"
#include "dashboard.h"
#include "cJSON.h"
#include "platform_path.h"
#include "platform_test_util.h"

static void test_vector_dashboard_falls_back_without_kb_service(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-dashboard-vector-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_setenv("AIMEE_KB_NO_AUTOSTART", "1");

   char *json = api_vector_status();
   assert(json != NULL);
   cJSON *obj = cJSON_Parse(json);
   assert(obj != NULL);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(obj, "status");
   cJSON *owner = cJSON_GetObjectItemCaseSensitive(obj, "owner");
   cJSON *available = cJSON_GetObjectItemCaseSensitive(obj, "available");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "unavailable") == 0);
   assert(cJSON_IsString(owner) && strcmp(owner->valuestring, "knowledge-service") == 0);
   assert(cJSON_IsBool(available) && cJSON_IsFalse(available));
   cJSON_Delete(obj);
   free(json);

   platform_test_rmrf(tmpdir);
}

int main(void)
{
   printf("test_dashboard\n");
   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;
   const char *old_kb_env = getenv("AIMEE_KB_NO_AUTOSTART");
   char *old_kb_no_autostart = old_kb_env ? strdup(old_kb_env) : NULL;

   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   test_vector_dashboard_falls_back_without_kb_service();

   if (old_home)
   {
      assert(platform_setenv("HOME", old_home) == 0);
      free(old_home);
   }
   else
   {
      assert(platform_unsetenv("HOME") == 0);
   }
   if (old_aimee_home)
   {
      assert(platform_setenv("AIMEE_HOME", old_aimee_home) == 0);
      free(old_aimee_home);
   }
   else
   {
      assert(platform_unsetenv("AIMEE_HOME") == 0);
   }
   if (old_no_cache)
   {
      assert(platform_setenv("AIMEE_NO_CACHE", old_no_cache) == 0);
      free(old_no_cache);
   }
   else
   {
      assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
   }
   if (old_kb_no_autostart)
   {
      assert(platform_setenv("AIMEE_KB_NO_AUTOSTART", old_kb_no_autostart) == 0);
      free(old_kb_no_autostart);
   }
   else
   {
      assert(platform_unsetenv("AIMEE_KB_NO_AUTOSTART") == 0);
   }

   printf("All dashboard tests passed.\n");
   return 0;
}
