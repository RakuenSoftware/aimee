/* manuscript.c: dependency-free helpers for `aimee manuscript`.
 *
 * Kept separate from cmd_manuscript.c (which pulls in run_cmd/cJSON) so these
 * pure helpers can be unit-tested in isolation. */
#include "manuscript.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Local path cap so this file stays dependency-free (no aimee.h, which pulls
 * db2 headers not on the thin client's include path). */
#define MS_PATH_MAX 4096

long manuscript_count_words(const char *text)
{
   if (!text)
      return 0;
   long n = 0;
   int in_word = 0;
   for (const char *p = text; *p; p++)
   {
      if (isspace((unsigned char)*p))
         in_word = 0;
      else if (!in_word)
      {
         in_word = 1;
         n++;
      }
   }
   return n;
}

int manuscript_is_prose_file(const char *name)
{
   if (!name || name[0] == '.')
      return 0;
   if (strcmp(name, ".aimee-rules") == 0)
      return 0;
   size_t n = strlen(name);
   if (n > 3 && strcmp(name + n - 3, ".md") == 0)
      return 1;
   if (n > 4 && strcmp(name + n - 4, ".txt") == 0)
      return 1;
   return 0;
}

int manuscript_continuity_failed(const char *report)
{
   return report && strstr(report, "CONTINUITY: FAIL") != NULL;
}

char *manuscript_read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len < 0 || len > 64L * 1024 * 1024) /* 64 MiB cap per scene */
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t nread = fread(buf, 1, (size_t)len, f);
   fclose(f);
   buf[nread] = '\0';
   return buf;
}

static int ms_entry_cmp(const void *a, const void *b)
{
   return strcmp(((const manuscript_entry_t *)a)->name, ((const manuscript_entry_t *)b)->name);
}

int manuscript_scan(const char *dir, manuscript_entry_t *out, int max, long *total_out)
{
   long total = 0;
   int count = 0;
   const char *scan_dir = (dir && dir[0]) ? dir : ".";
   DIR *d = opendir(scan_dir);
   if (d)
   {
      struct dirent *e;
      while ((e = readdir(d)) != NULL && count < max)
      {
         if (!manuscript_is_prose_file(e->d_name))
            continue;
         size_t name_len = strlen(e->d_name);
         if (name_len >= sizeof(out[count].name))
            continue;
         char path[MS_PATH_MAX];
         snprintf(path, sizeof(path), "%s/%s", scan_dir, e->d_name);
         char *text = manuscript_read_file(path);
         long w = manuscript_count_words(text);
         free(text);
         memcpy(out[count].name, e->d_name, name_len + 1);
         out[count].words = w;
         total += w;
         count++;
      }
      closedir(d);
   }
   qsort(out, (size_t)count, sizeof(out[0]), ms_entry_cmp);
   if (total_out)
      *total_out = total;
   return count;
}
