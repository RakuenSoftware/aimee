/* test_bus_audit_retention.c: the audit capture stream is retained across
 * sessions, but bounded.
 *
 * Each host session writes its own capture file (a new host restarts seq, so the
 * streams cannot share a file). A restart must therefore NOT clobber the prior
 * session's replayable record — but the files must not accumulate without limit
 * either. This test pre-seeds more old capture files than the retention bound,
 * runs a real audit session (which creates one more and prunes), and requires
 * that exactly the retention bound survives AND that the live session's own file
 * is one of the survivors.
 */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aimee/audit/obs_bus.h>
#include "log.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define KEEP 16 /* must match AB_CAP_KEEP in obs_bus.c */

static int count_capture_files(const char *dir)
{
   DIR *d = opendir(dir);
   assert(d);
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
      if (strncmp(e->d_name, "audit-bus-capture-", 18) == 0 && strstr(e->d_name, ".aimeecap"))
         n++;
   closedir(d);
   return n;
}

/* Is there a capture file whose size is > 64 bytes (i.e. more than just a header)?
 * The live session wrote real records, the pre-seeded stubs are tiny — so a
 * survivor bigger than a header proves the LIVE file was retained, not pruned. */
static int has_nonempty_capture(const char *dir)
{
   DIR *d = opendir(dir);
   assert(d);
   int found = 0;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strncmp(e->d_name, "audit-bus-capture-", 18) == 0 && strstr(e->d_name, ".aimeecap"))
      {
         char p[4096];
         snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
         struct stat st;
         if (stat(p, &st) == 0 && st.st_size > 64)
            found = 1;
      }
   }
   closedir(d);
   return found;
}

int main(void)
{
   printf("test_bus_audit_retention:\n");

   char home[256];
   snprintf(home, sizeof home, "%s/aimee-busret-XXXXXX", platform_tmpdir());
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();

   /* Pre-seed KEEP+5 OLD session files (tiny timestamps so they sort oldest and
    * are the ones pruned). */
   const int seeded = KEEP + 5;
   for (int i = 0; i < seeded; i++)
   {
      char p[4096];
      snprintf(p, sizeof p, "%s/audit-bus-capture-%010d-0-%03d.aimeecap", home, i, i);
      int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      assert(fd >= 0);
      (void)!write(fd, "old", 3);
      close(fd);
   }
   printf("  seeded %d old capture files (retention bound %d)\n", seeded, KEEP);

   /* Run a real audit session: it creates one live file and prunes to KEEP. */
   if (obs_bus_start() != 0)
   {
      fprintf(stderr, "FAIL: obs_bus_start\n");
      return 1;
   }
   for (int i = 0; i < 200; i++)
      obs_bus_emit("primary", "Write", "v1-x", "cd ; rm", "approve", "reason", "block", i);
   obs_bus_stop();

   int remaining = count_capture_files(home);
   printf("  after a session + prune: %d capture files remain\n", remaining);
   if (remaining != KEEP)
   {
      fprintf(stderr, "FAIL: retention kept %d files, expected exactly %d\n", remaining, KEEP);
      return 1;
   }
   if (!has_nonempty_capture(home))
   {
      fprintf(stderr, "FAIL: the live session's own capture file was pruned (no non-stub file)\n");
      return 1;
   }

   /* Ordering: the pruned files must be the OLDEST, not an arbitrary set. Seeded
    * files sort (by their tiny timestamps) before the live file, so with
    * seeded+1=KEEP+6 files pruned to KEEP, exactly the (seeded-KEEP+1) lowest
    * seed indices are removed and the higher ones survive. */
   const int pruned = (seeded + 1) - KEEP; /* live file + seeded, minus the kept bound */
   for (int i = 0; i < seeded; i++)
   {
      char p[4096];
      snprintf(p, sizeof p, "%s/audit-bus-capture-%010d-0-%03d.aimeecap", home, i, i);
      int exists = (access(p, F_OK) == 0);
      int should_exist = (i >= pruned); /* the `pruned` oldest are gone */
      if (exists != should_exist)
      {
         fprintf(stderr, "FAIL: seed %d %s but should %s (retention pruned the wrong files)\n", i,
                 exists ? "survived" : "was pruned", should_exist ? "survive" : "be pruned");
         return 1;
      }
   }
   printf("  the live session's file survived; the %d OLDEST seeds pruned, newer ones kept\n",
          pruned);

   printf("test_bus_audit_retention: OK (capture streams retained across restarts, bounded)\n");
   return 0;
}
