/* harness_memory_spill.c — see harness_memory_spill.h. */

#include "harness_memory_spill.h"

#include "aimee_home.h" /* aimee_home() — honors AIMEE_HOME/AIMEE_PROFILE */
#include "cJSON.h"
#include "harness_memory_common.h" /* hmem_sha256_hex */
#include "platform_path.h"         /* platform_mkdir_p (portable mkdir -p) */

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

static int spill_home(char *out, size_t cap)
{
   const char *h = aimee_home();
   if (!h || !h[0])
      return -1;
   return ((size_t)snprintf(out, cap, "%s", h) < cap) ? 0 : -1;
}

int hmem_spill_dir(const char *project, char *out, size_t cap)
{
   if (!project || !project[0])
      return -1;
   char home[PATH_MAX];
   if (spill_home(home, sizeof(home)) != 0)
      return -1;
   char ph[HMEM_HASH_HEX_LEN];
   hmem_sha256_hex(project, strlen(project), ph);
   char base[PATH_MAX];
   if ((size_t)snprintf(base, sizeof(base), "%s/harness_spill", home) >= sizeof(base))
      return -1;
   platform_mkdir_p(home, 0700);
   platform_mkdir_p(base, 0700);
   if ((size_t)snprintf(out, cap, "%s/%s", base, ph) >= cap)
      return -1;
   platform_mkdir_p(out, 0700);
   return 0;
}

int hmem_spill_write(const char *project, const char *name, const char *type, const char *body)
{
   if (!project || !name)
      return -1;
   char dir[PATH_MAX];
   if (hmem_spill_dir(project, dir, sizeof(dir)) != 0)
      return -1;

   char ts[32];
   time_t t = time(NULL);
   struct tm tm_buf = {0};
   struct tm *gmt = gmtime(&t); /* gmtime_r is POSIX-only; CLI is single-threaded */
   if (gmt)
      tm_buf = *gmt;
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

   cJSON *o = cJSON_CreateObject();
   if (!o)
      return -1;
   cJSON_AddStringToObject(o, "op", "upsert");
   cJSON_AddNumberToObject(o, "schema_version", 1);
   cJSON_AddStringToObject(o, "project", project);
   cJSON_AddStringToObject(o, "name", name);
   cJSON_AddStringToObject(o, "type", (type && type[0]) ? type : "fact");
   cJSON_AddStringToObject(o, "body", body ? body : "");
   cJSON_AddStringToObject(o, "ts", ts);
   char *s = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   if (!s)
      return -1;
   size_t len = strlen(s);

#ifndef _WIN32
   char tmpl[PATH_MAX];
   if ((size_t)snprintf(tmpl, sizeof(tmpl), "%s/.spill_XXXXXX", dir) >= sizeof(tmpl))
   {
      free(s);
      return -1;
   }
   int fd = mkstemp(tmpl);
   if (fd < 0)
   {
      free(s);
      return -1;
   }
   /* Commit under a collision-resistant name reusing mkstemp's unique suffix —
    * a deterministic pid/counter name could overwrite an unconsumed spill. */
   const char *rnd = strrchr(tmpl, '_');
   rnd = rnd ? rnd + 1 : tmpl;
   char target[PATH_MAX];
   if ((size_t)snprintf(target, sizeof(target), "%s/spill-%s.json", dir, rnd) >= sizeof(target))
   {
      close(fd);
      unlink(tmpl);
      free(s);
      return -1;
   }
   ssize_t w = write(fd, s, len);
   fsync(fd);
   close(fd);
   free(s);
   if (w < 0 || (size_t)w != len || rename(tmpl, target) != 0)
   {
      unlink(tmpl);
      return -1;
   }
   /* fsync the directory so the rename survives power loss (otherwise it can
    * revert to the dotfile temp name, which the consumer skips → silent loss). */
   int dfd = open(dir, O_RDONLY | O_DIRECTORY);
   if (dfd >= 0)
   {
      fsync(dfd);
      close(dfd);
   }
   return 0;
#else
   char target[PATH_MAX];
   if ((size_t)snprintf(target, sizeof(target), "%s/spill-%lu-%lu.json", dir, (unsigned long)t,
                        (unsigned long)len) >= sizeof(target))
   {
      free(s);
      return -1;
   }
   FILE *f = fopen(target, "wb");
   if (!f)
   {
      free(s);
      return -1;
   }
   size_t wr = fwrite(s, 1, len, f);
   fclose(f);
   free(s);
   return (wr == len) ? 0 : -1;
#endif
}
