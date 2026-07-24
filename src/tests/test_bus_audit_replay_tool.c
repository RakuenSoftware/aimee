/* test_bus_audit_replay_tool.c: the operator replay printer (audit_replay.c),
 * which backs `aimee-server --audit-replay <file>`.
 *
 * The bus-level replay is proven elsewhere (test_bus_audit_replay); this covers
 * the OPERATOR path: run a real audit session, then feed its capture file to
 * audit_bus_replay_print and require the rendered output to name every recorded
 * governed-action row and report the stream status.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audit_bus.h"
#include "audit_replay.h"
#include "log.h"

#define ROWS 50

int main(void)
{
   printf("test_bus_audit_replay_tool:\n");

   char home[] = "/tmp/aimee-busreplaytool-XXXXXX";
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();

   assert(audit_bus_start() == 0);
   for (int i = 0; i < ROWS; i++)
   {
      char tool[32];
      snprintf(tool, sizeof tool, "Tool_%d", i % 7);
      audit_bus_emit("primary", tool, "v1-x", "cd ; rm", "approve", "read_before_write",
                     (i % 2) ? "block" : "allow", i);
   }
   audit_bus_stop();

   /* Locate the session capture file. */
   char path[4096];
   path[0] = '\0';
   DIR *d = opendir(home);
   assert(d);
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
      if (strncmp(e->d_name, "audit-bus-capture-", 18) == 0 && strstr(e->d_name, ".aimeecap"))
      {
         snprintf(path, sizeof path, "%s/%s", home, e->d_name);
         break;
      }
   closedir(d);
   assert(path[0]);

   /* Render the replay into memory and check it. */
   char *obuf = NULL;
   size_t osz = 0;
   FILE *m = open_memstream(&obuf, &osz);
   assert(m);
   int rc = audit_bus_replay_print(path, m);
   fclose(m);

   if (rc != 0)
   {
      fprintf(stderr, "FAIL: audit_bus_replay_print returned %d for a valid stream\n", rc);
      return 1;
   }
   char needle[64];
   snprintf(needle, sizeof needle, "%d governed-action row(s) replayed", ROWS);
   if (!strstr(obuf, needle) || !strstr(obuf, "task_id=0 ") || !strstr(obuf, "task_id=49 ") ||
       !strstr(obuf, "verdict=block") || !strstr(obuf, "verdict=allow"))
   {
      fprintf(stderr, "FAIL: replay output did not render the expected rows/summary:\n%s\n", obuf);
      return 1;
   }
   printf("  rendered %d rows + status trailer; first/last task ids and both verdicts present\n",
          ROWS);
   free(obuf);

   /* NULL out: classify without printing, still valid. */
   if (audit_bus_replay_print(path, NULL) != 0)
   {
      fprintf(stderr, "FAIL: classify-only pass rejected a valid stream\n");
      return 1;
   }

   /* A missing file is a clean error, not a crash. */
   if (audit_bus_replay_print("/no/such/capture.aimeecap", NULL) != -1)
   {
      fprintf(stderr, "FAIL: a missing capture file should return -1\n");
      return 1;
   }

   printf("test_bus_audit_replay_tool: OK (the operator replay tool renders the recorded rows)\n");
   return 0;
}
