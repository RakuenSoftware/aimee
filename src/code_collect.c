/* code_collect.c: see code_collect.h. */
#include "platform.h" /* AIMEE_POSIX */
#include "code_collect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIMEE_POSIX
#include <dirent.h>
#include <sys/stat.h>

static int code_ext_ok(const char *name)
{
   static const char *const exts[] = {".c",    ".h",   ".cc",    ".cpp",  ".cxx", ".m",    ".py",
                                      ".go",   ".rs",  ".ts",    ".tsx",  ".js",  ".jsx",  ".md",
                                      ".yaml", ".yml", ".toml",  ".json", ".sh",  ".bash", ".rb",
                                      ".java", ".kt",  ".swift", NULL};
   const char *dot = strrchr(name, '.');
   if (!dot || dot == name)
      return 0;
   for (int i = 0; exts[i]; i++)
      if (strcmp(dot, exts[i]) == 0)
         return 1;
   return 0;
}

static int code_dir_skip(const char *name)
{
   static const char *const skip[] = {"node_modules", "__pycache__", "vendor", "target",
                                      "build",        "dist",        "out",    NULL};
   if (name[0] == '.') /* .git, .aimee, .cache, .svn, hidden dirs */
      return 1;
   for (int i = 0; skip[i]; i++)
      if (strcmp(name, skip[i]) == 0)
         return 1;
   return 0;
}

/* Walk root/rel recursively, appending {"rel_path","content"} entries to
 * files_arr. Stops at CODE_COLLECT_MAX_FILES. */
static void code_collect_walk(const char *root, const char *rel, cJSON *files_arr, int *count)
{
   char path[4096];
   if (rel && rel[0])
      snprintf(path, sizeof(path), "%s/%s", root, rel);
   else
      snprintf(path, sizeof(path), "%s", root);

   DIR *dir = opendir(path);
   if (!dir)
      return;

   struct dirent *ent;
   while (*count < CODE_COLLECT_MAX_FILES && (ent = readdir(dir)) != NULL)
   {
      if (ent->d_name[0] == '.' &&
          (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
         continue; /* skip . and .. */

      char full[4096];
      snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

      struct stat st;
      if (stat(full, &st) != 0)
         continue;

      char rel_child[4096];
      if (rel && rel[0])
         snprintf(rel_child, sizeof(rel_child), "%s/%s", rel, ent->d_name);
      else
         snprintf(rel_child, sizeof(rel_child), "%s", ent->d_name);

      if (S_ISDIR(st.st_mode))
      {
         if (!code_dir_skip(ent->d_name))
            code_collect_walk(root, rel_child, files_arr, count);
      }
      else if (S_ISREG(st.st_mode))
      {
         if (!code_ext_ok(ent->d_name))
            continue;
         if (st.st_size <= 0 || st.st_size > CODE_COLLECT_MAX_FILE_BYTES)
            continue;

         FILE *fp = fopen(full, "rb");
         if (!fp)
            continue;
         size_t sz = (size_t)st.st_size;
         char *buf = malloc(sz + 1);
         if (!buf)
         {
            fclose(fp);
            continue;
         }
         size_t got = fread(buf, 1, sz, fp);
         fclose(fp);
         if (got != sz)
         {
            free(buf);
            continue;
         }
         buf[sz] = '\0';

         /* Skip binary files by scanning for null bytes. */
         int binary = 0;
         for (size_t i = 0; i < sz && !binary; i++)
            if ((unsigned char)buf[i] == '\0')
               binary = 1;
         if (binary)
         {
            free(buf);
            continue;
         }

         cJSON *entry = cJSON_CreateObject();
         if (entry)
         {
            cJSON_AddStringToObject(entry, "rel_path", rel_child);
            cJSON_AddStringToObject(entry, "content", buf);
            cJSON_AddItemToArray(files_arr, entry);
            (*count)++;
         }
         free(buf);
      }
   }
   closedir(dir);
}

int code_collect_files(const char *root, cJSON *files_arr)
{
   if (!root || !root[0] || !files_arr)
      return 0;
   int count = 0;
   code_collect_walk(root, NULL, files_arr, &count);
   return count;
}

#else /* !AIMEE_POSIX */

int code_collect_files(const char *root, cJSON *files_arr)
{
   (void)root;
   (void)files_arr;
   return 0;
}

#endif /* AIMEE_POSIX */
