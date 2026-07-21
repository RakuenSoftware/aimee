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
#include "db.h"
#include "db_schema.h"
#include "dashboard.h"
#include "cJSON.h"
#if AIMEE_WITH_PLUGIN_LOADER
#include "aimee/plugin-loader/plugin.h"
#endif
#include "platform_path.h"
#include "platform_test_util.h"

#if AIMEE_WITH_PLUGIN_LOADER
static void write_manifest(const char *plugin_dir, const char *json)
{
   char manifest_dir[512];
   snprintf(manifest_dir, sizeof(manifest_dir), "%s/.aimee-plugin", plugin_dir);
   mkdir(manifest_dir, 0755);

   char manifest_path[512];
   snprintf(manifest_path, sizeof(manifest_path), "%s/plugin.json", manifest_dir);
   FILE *f = fopen(manifest_path, "w");
   assert(f != NULL);
   fputs(json, f);
   fclose(f);
}

static void test_plugin_dashboard_api(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-dashboard-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);

   char plugin_dir[512];
   snprintf(plugin_dir, sizeof(plugin_dir), "%s/plugin-dashboard", tmpdir);
   mkdir(plugin_dir, 0755);
   write_manifest(plugin_dir, "{"
                              "  \"name\": \"dashboard-plugin\","
                              "  \"version\": \"1.0.0\","
                              "  \"description\": \"Plugin used by dashboard test\","
                              "  \"defaultEnabled\": true,"
                              "  \"hooks\": {"
                              "    \"PreToolUse\": [\"/bin/true\", \"/bin/false\"]"
                              "  },"
                              "  \"tools\": ["
                              "    {"
                              "      \"name\": \"dashboard_tool\","
                              "      \"description\": \"tool for dashboard test\","
                              "      \"command\": \"/bin/echo\","
                              "      \"permission\": \"read\""
                              "    }"
                              "  ]"
                              "}");

   char err[256];
   assert(plugin_install(plugin_dir, err, sizeof(err)) == 0);

   char audit_dir[512];
   snprintf(audit_dir, sizeof(audit_dir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(audit_dir, 0700) == 0);

   char audit_path[512];
   snprintf(audit_path, sizeof(audit_path), "%s/audit.log", audit_dir);
   FILE *audit = fopen(audit_path, "w");
   assert(audit != NULL);
   /* The 24h-window counters depend on real time, so compute fixture
    * timestamps relative to now rather than hard-coding dates that
    * eventually slide out of the window. */
   time_t now = time(NULL);
   time_t inside_window = now - 3600;        /* 1 hour ago */
   time_t outside_window = now - 86400 * 30; /* 30 days ago */
   char inside_ts[32], outside_ts[32];
   struct tm tm_buf;
   gmtime_r(&inside_window, &tm_buf);
   strftime(inside_ts, sizeof(inside_ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   gmtime_r(&outside_window, &tm_buf);
   strftime(outside_ts, sizeof(outside_ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   fprintf(audit,
           "{\"ts\":\"%s\",\"event\":\"plugin-hook\",\"detail\":\"phase=PreToolUse "
           "rc=0 hook=/bin/true\"}\n",
           inside_ts);
   fprintf(audit,
           "{\"ts\":\"%s\",\"event\":\"plugin-hook\",\"detail\":\"phase=PreToolUse "
           "rc=7 hook=/bin/false\"}\n",
           inside_ts);
   fprintf(audit,
           "{\"ts\":\"%s\",\"event\":\"plugin-hook\",\"detail\":\"phase=PreToolUse "
           "rc=9 hook=/bin/old\"}\n",
           outside_ts);
   fclose(audit);

   char *plugins_json = api_dashboard_plugins();
   assert(plugins_json != NULL);
   cJSON *plugins = cJSON_Parse(plugins_json);
   assert(plugins != NULL);
   assert(cJSON_GetObjectItem(plugins, "installed")->valueint == 1);
   assert(cJSON_GetObjectItem(plugins, "enabled")->valueint == 1);
   assert(cJSON_GetObjectItem(plugins, "hooks_total")->valueint == 2);
   assert(cJSON_GetObjectItem(plugins, "tools_total")->valueint == 1);
   assert(cJSON_GetObjectItem(plugins, "hook_runs_24h")->valueint == 2);
   assert(cJSON_GetObjectItem(plugins, "hook_failures_24h")->valueint == 1);
   cJSON *recent_failures = cJSON_GetObjectItem(plugins, "recent_failures");
   assert(cJSON_IsArray(recent_failures));
   assert(cJSON_GetArraySize(recent_failures) == 1);
   cJSON *failure = cJSON_GetArrayItem(recent_failures, 0);
   assert(strcmp(cJSON_GetObjectItem(failure, "hook")->valuestring, "/bin/false") == 0);
   cJSON_Delete(plugins);
   free(plugins_json);

   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);
   char *logs_json = api_logs();
   assert(logs_json != NULL);
   cJSON *logs = cJSON_Parse(logs_json);
   assert(logs != NULL);
   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(logs); i++)
   {
      cJSON *row = cJSON_GetArrayItem(logs, i);
      cJSON *source = cJSON_GetObjectItem(row, "source");
      cJSON *tag = cJSON_GetObjectItem(row, "tag");
      if (cJSON_IsString(source) && cJSON_IsString(tag) &&
          strcmp(source->valuestring, "audit") == 0 && strcmp(tag->valuestring, "plugin-hook") == 0)
      {
         found = 1;
         break;
      }
   }
   assert(found);
   cJSON_Delete(logs);
   free(logs_json);
   sqlite3_close(db);

   platform_test_rmrf(tmpdir);
}
#endif

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

#if AIMEE_WITH_PLUGIN_LOADER
   test_plugin_dashboard_api();
#endif
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
