/* test_bus_audit_replay_tool.c: the operator replay printer (audit_replay.c),
 * which backs `aimee-server --audit-replay <file>`.
 *
 * The bus-level replay is proven elsewhere (test_bus_audit_replay); this covers
 * the OPERATOR path: run a real audit session, then feed its capture file to
 * obs_bus_replay_print and require the rendered output to name every recorded
 * governed-action row and report the stream status.
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <aimee/audit/obs_bus.h>
#include <aimee/audit/audit_replay.h>
#include "cJSON.h"
#include "log.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define ROWS 50

int main(void)
{
   printf("test_bus_audit_replay_tool:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busreplaytool-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();

   assert(obs_bus_start() == 0);
   for (int i = 0; i < ROWS; i++)
   {
      char tool[32];
      snprintf(tool, sizeof tool, "Tool_%d", i % 7);
      obs_bus_emit("primary", tool, "v1-x", "cd ; rm", "approve", "read_before_write",
                   (i % 2) ? "block" : "allow", i);
   }
   obs_bus_stop();

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
   int rc = obs_bus_replay_print(path, m);
   fclose(m);

   if (rc != 0)
   {
      fprintf(stderr, "FAIL: obs_bus_replay_print returned %d for a valid stream\n", rc);
      return 1;
   }
   char needle[64];
   snprintf(needle, sizeof needle, "%d governed-action row(s) replayed", ROWS);
   /* Count every rendered row, not just spot-check endpoints: a printer that
    * dropped middle rows would pass a first/last check but fail this. */
   int rendered = 0;
   for (const char *p = obuf; (p = strstr(p, "task_id=")) != NULL; p += 8)
      rendered++;
   if (!strstr(obuf, needle) || rendered != ROWS || !strstr(obuf, "task_id=0 ") ||
       !strstr(obuf, "task_id=49 ") || !strstr(obuf, "verdict=block") ||
       !strstr(obuf, "verdict=allow"))
   {
      fprintf(stderr,
              "FAIL: replay rendered %d task_id lines (expected %d) / missing summary:\n%s\n",
              rendered, ROWS, obuf);
      return 1;
   }
   printf("  rendered %d rows + status trailer; first/last task ids and both verdicts present\n",
          ROWS);
   free(obuf);

   /* NULL out: classify without printing, still valid. */
   if (obs_bus_replay_print(path, NULL) != 0)
   {
      fprintf(stderr, "FAIL: classify-only pass rejected a valid stream\n");
      return 1;
   }

   /* A missing file is a clean error, not a crash. */
   if (obs_bus_replay_print("/no/such/capture.aimeecap", NULL) != -1)
   {
      fprintf(stderr, "FAIL: a missing capture file should return -1\n");
      return 1;
   }

   /* JSON producers used by the /v1/audit endpoints. */
   cJSON *j = audit_replay_to_json(path, 0, 1000);
   assert(j);
   assert((int)cJSON_GetObjectItem(j, "total")->valuedouble == ROWS);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(j, "rows")) == ROWS);
   cJSON *r0 = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "rows"), 0);
   assert(cJSON_GetObjectItem(r0, "verdict") && cJSON_GetObjectItem(r0, "task_id"));
   cJSON_Delete(j);

   /* Pagination: total counts every row, only the window is materialized, and the
    * window starts at the requested offset (rows are in emit/seq order). */
   cJSON *jp = audit_replay_to_json(path, 10, 5);
   assert((int)cJSON_GetObjectItem(jp, "total")->valuedouble == ROWS);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(jp, "rows")) == 5);
   assert((int)cJSON_GetObjectItem(jp, "offset")->valuedouble == 10);
   cJSON *w0 = cJSON_GetArrayItem(cJSON_GetObjectItem(jp, "rows"), 0);
   assert((int)cJSON_GetObjectItem(w0, "task_id")->valuedouble == 10);
   cJSON_Delete(jp);

   /* Path-traversal guard: only a bare capture basename is accepted. */
   assert(audit_replay_valid_basename("audit-bus-capture-1-2-3.aimeecap"));
   assert(!audit_replay_valid_basename("../etc/passwd"));
   assert(!audit_replay_valid_basename("audit-bus-capture-/x.aimeecap"));
   assert(!audit_replay_valid_basename("foo.txt"));
   assert(!audit_replay_valid_basename(".aimeecap"));

   /* The capture list names our session's file. */
   cJSON *list = audit_replay_capture_list(home);
   assert(cJSON_GetArraySize(list) >= 1);
   cJSON_Delete(list);
   printf("  JSON replay + pagination + basename guard + capture list verified\n");

   /* Byte budget (regression for the /v1 256 KB overflow): a large capture must
    * be PAGED even with a big row limit, never materialized past the budget. */
   const int BIG = 2500;         /* ~150 B/row >> the 200 KB budget */
   assert(obs_bus_start() == 0); /* fresh session (clears the terminated guard) */
   for (int i = 0; i < BIG; i++)
   {
      char t[32], h[32];
      snprintf(t, sizeof t, "Tool_%d", i % 7);
      snprintf(h, sizeof h, "v1-%d", i);
      obs_bus_emit("primary", t, h, "cd ; rm", "approve", "read_before_write", "block", i);
   }
   obs_bus_stop();
   /* Pick the largest capture file (the BIG session's). */
   char big[4096];
   big[0] = '\0';
   long best = -1;
   DIR *bd = opendir(home);
   assert(bd);
   while ((e = readdir(bd)) != NULL)
   {
      if (strncmp(e->d_name, "audit-bus-capture-", 18) != 0 || !strstr(e->d_name, ".aimeecap"))
         continue;
      char p[4096];
      snprintf(p, sizeof p, "%s/%s", home, e->d_name);
      struct stat st;
      if (stat(p, &st) == 0 && (long)st.st_size > best)
      {
         best = st.st_size;
         snprintf(big, sizeof big, "%s", p);
      }
   }
   closedir(bd);
   assert(big[0]);
   cJSON *jb = audit_replay_to_json(big, 0, 100000); /* huge limit — budget must still bound it */
   assert(jb);
   assert((int)cJSON_GetObjectItem(jb, "total")->valuedouble == BIG);
   int cnt = (int)cJSON_GetObjectItem(jb, "count")->valuedouble;
   if (cnt >= BIG || cnt <= 0 || !cJSON_IsTrue(cJSON_GetObjectItem(jb, "truncated")))
   {
      fprintf(stderr, "FAIL: byte budget did not page a large capture (count=%d total=%d)\n", cnt,
              BIG);
      return 1;
   }
   char *js = cJSON_PrintUnformatted(jb);
   assert(js);
   if (strlen(js) >= 256 * 1024)
   {
      fprintf(stderr, "FAIL: paged replay JSON is %zu bytes — would overflow the /v1 buffer\n",
              strlen(js));
      return 1;
   }
   free(js);
   cJSON_Delete(jb);
   printf("  byte budget: %d-row capture paged to %d rows (truncated), JSON < 256 KB\n", BIG, cnt);

   printf("test_bus_audit_replay_tool: OK (the operator replay tool renders the recorded rows)\n");
   return 0;
}
