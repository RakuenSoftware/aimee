/* harness_memory_audit.c — see harness_memory_audit.h. */

#include "harness_memory_audit.h"

#include "aimee_home.h" /* aimee_home() — honors AIMEE_HOME/AIMEE_PROFILE */
#include "cJSON.h"
#include "platform_path.h" /* platform_mkdir_p (portable mkdir -p) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int audit_home(char *out, size_t cap)
{
   const char *h = aimee_home();
   if (!h || !h[0])
      return -1;
   return ((size_t)snprintf(out, cap, "%s", h) < cap) ? 0 : -1;
}

void hmem_audit(const char *action, const char *project, const char *name, const char *detail)
{
   if (!action)
      return;
   char home[PATH_MAX];
   if (audit_home(home, sizeof(home)) != 0)
      return;

   char logdir[PATH_MAX], logpath[PATH_MAX];
   if ((size_t)snprintf(logdir, sizeof(logdir), "%s/logs", home) >= sizeof(logdir))
      return;
   platform_mkdir_p(home, 0700);
   platform_mkdir_p(logdir, 0700);
   if ((size_t)snprintf(logpath, sizeof(logpath), "%s/interception.jsonl", logdir) >=
       sizeof(logpath))
      return;

   char ts[32];
   time_t t = time(NULL);
   struct tm tm_buf = {0};
   struct tm *gmt = gmtime(&t); /* gmtime_r is POSIX-only; CLI is single-threaded */
   if (gmt)
      tm_buf = *gmt;
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

   cJSON *o = cJSON_CreateObject();
   if (!o)
      return;
   cJSON_AddStringToObject(o, "ts", ts);
   cJSON_AddStringToObject(o, "action", action);
   if (project)
      cJSON_AddStringToObject(o, "project", project);
   if (name)
      cJSON_AddStringToObject(o, "name", name);
   if (detail)
      cJSON_AddStringToObject(o, "detail", detail);
   char *line = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   if (!line)
      return;

   /* Create 0600 atomically — no world-readable window on this sensitive log. */
#ifndef _WIN32
   int flags = O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC;
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   int fd = open(logpath, flags, 0600);
   if (fd < 0)
   {
      free(line);
      return;
   }
   FILE *f = fdopen(fd, "a");
   if (!f)
   {
      close(fd);
      free(line);
      return;
   }
#else
   FILE *f = fopen(logpath, "a");
   if (!f)
   {
      free(line);
      return;
   }
#endif
   fprintf(f, "%s\n", line);
   fclose(f);
   free(line);
}
