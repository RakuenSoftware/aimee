/* Capture is diagnostic, but every period in which it is absent is durable. */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <aimee/audit/audit_worm.h>
#include <aimee/audit/obs_bus.h>

#include "cJSON.h"
#include "platform_test_util.h"

static int worm_sink(const char *role, const char *principal, const char *action,
                     const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return audit_worm_append(role, principal, action, subject, verdict, detail);
}

static void wait_reason(const char *reason)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int i = 0; i < 5000; ++i)
   {
      obs_bus_capture_health_t health;
      obs_bus_capture_health(&health);
      if (!health.capture_ok && strcmp(health.reason, reason) == 0)
         return;
      nanosleep(&pause, NULL);
   }
   fprintf(stderr, "FAIL: capture reason never became %s\n", reason);
   abort();
}

static int rows_matching(const char *action, const char *reason)
{
   long total = 0;
   cJSON *rows = audit_worm_read_page(0, 500, &total);
   assert(rows && total >= 0);
   int matches = 0;
   const cJSON *row = NULL;
   cJSON_ArrayForEach(row, rows)
   {
      const cJSON *a = cJSON_GetObjectItemCaseSensitive(row, "action");
      const cJSON *detail = cJSON_GetObjectItemCaseSensitive(row, "detail");
      if (cJSON_IsString(a) && strcmp(a->valuestring, action) == 0 &&
          (!reason || (cJSON_IsString(detail) && strstr(detail->valuestring, reason))))
         matches++;
   }
   cJSON_Delete(rows);
   return matches;
}

static int capture_file_count(const char *home)
{
   DIR *directory = opendir(home);
   assert(directory);
   int count = 0;
   struct dirent *entry;
   while ((entry = readdir(directory)) != NULL)
      if (strncmp(entry->d_name, "audit-bus-capture-", 18) == 0 &&
          strstr(entry->d_name, ".aimeecap"))
         count++;
   closedir(directory);
   return count;
}

static void induce_gap(const char *reason, int needs_event)
{
   assert(obs_bus_test_capture_fault(reason) == 0);
   assert(obs_bus_start() == 0);
   if (needs_event)
      obs_bus_emit_durable_event("test.event", "capture-gap-test", "ok", "{}");
   wait_reason(reason);
   obs_bus_stop();
   assert(rows_matching("bus.capture.gap", reason) == 1);
   printf("  %s: durable capture-gap row present\n", reason);
}

static void test_prune_marker(const char *home)
{
   assert(obs_bus_test_capture_fault(NULL) == 0);
   int rows_before = rows_matching("bus.capture.pruned", NULL);
   for (int i = 0; i < 18; ++i)
   {
      char path[512];
      snprintf(path, sizeof path, "%s/audit-bus-capture-%010d-0-%03d.aimeecap", home, i, i);
      int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      assert(fd >= 0);
      assert(write(fd, "old", 3) == 3);
      close(fd);
   }
   int files_before = capture_file_count(home);
   assert(obs_bus_start() == 0);
   obs_bus_stop();
   int expected = files_before + 1 - 16;
   assert(expected > 0);
   assert(rows_matching("bus.capture.pruned", NULL) - rows_before == expected);
   printf("  prune: each removed session left a durable row\n");
}

int main(void)
{
   char home[256], worm_path[320];
   snprintf(home, sizeof home, "%s/aimee-capture-gap-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home));
   setenv("AIMEE_HOME", home, 1);
   snprintf(worm_path, sizeof worm_path, "%s/worm.db", home);
   assert(audit_worm_init_at(worm_path) == 0);
   assert(obs_bus_set_durable_sink(worm_sink, NULL) == 0);

   induce_gap("no_home", 0);
   induce_gap("open_failed", 0);
   induce_gap("write_failed", 1);
   induce_gap("sink_broken", 0);
   test_prune_marker(home);

   assert(audit_worm_verify_chain(NULL, 0) == 0);
   audit_worm_close();
   puts("test_bus_capture_gaps: OK");
   return 0;
}
