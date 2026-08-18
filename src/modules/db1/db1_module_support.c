/* db1_module_support.c: the two primitives the DB1 module process needs.
 *
 * Same reason db1_time.c exists, and the same rule: this file is in the module
 * descriptor and deliberately NOT in DB1_SRCS. The daemon gets both of these
 * from log.c and posix/platform_path.c, so a second definition there would be a
 * duplicate symbol; the module links the domain without either, so without this
 * one they are undefined symbols.
 *
 * They only became undefined when the runtime family was served: --gc-sections
 * had been dropping fsnap's directory creation and db.c's logging because
 * nothing reachable from main called them. Declaring an operation makes them
 * reachable, so the gap was deferred rather than absent.
 *
 * The logger writes to stderr rather than to the daemon's log file. A module is
 * a separate process whose output the supervisor collects; writing into the
 * daemon's file from here would interleave two processes' lines with no way to
 * tell them apart.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "aimee.h"
#include "log.h"
#include "platform_path.h"

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
   size_t at = (size_t)level;
   fprintf(stderr, "aimee-module-db1 %s %s: ",
           at < sizeof names / sizeof names[0] ? names[at] : "LOG", module ? module : "db1");
   va_list args;
   va_start(args, fmt);
   vfprintf(stderr, fmt, args);
   va_end(args);
   fputc('\n', stderr);
}

int platform_mkdir_p(const char *path, int mode)
{
   char tmp[4096];
   if (!path)
      return -1;
   snprintf(tmp, sizeof(tmp), "%s", path);
   size_t len = strlen(tmp);
   if (len == 0)
      return -1;
   if (tmp[len - 1] == '/')
      tmp[len - 1] = '\0';

   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
            return -1;
         *p = '/';
      }
   }
   if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
      return -1;
   return 0;
}
