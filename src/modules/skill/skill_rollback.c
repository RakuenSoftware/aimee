/* skill_rollback.c: restore project skills from local snapshots */
#include "skill.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void rollback_err(char *errbuf, size_t errbuf_len, const char *msg)
{
   if (errbuf && errbuf_len > 0)
      snprintf(errbuf, errbuf_len, "%s", msg ? msg : "skill rollback failed");
}

static int snapshot_id_ok(const char *snapshot_id)
{
   if (!snapshot_id || !snapshot_id[0] || strlen(snapshot_id) > 128 ||
       strcmp(snapshot_id, ".") == 0 || strcmp(snapshot_id, "..") == 0 ||
       strstr(snapshot_id, "..") != NULL)
      return 0;
   for (const char *p = snapshot_id; *p; p++)
      if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.')
         return 0;
   return 1;
}

static int rollback_lstat(const char *path, struct stat *st)
{
#ifdef _WIN32
   return stat(path, st);
#else
   return lstat(path, st);
#endif
}

static int mkdir_p_local(const char *path)
{
   if (!path || !path[0])
      return -1;
   char tmp[SKILL_PATH_MAX];
   snprintf(tmp, sizeof(tmp), "%s", path);
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return -1;
         *p = '/';
      }
   }
   if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
      return -1;
   return 0;
}

static int remove_tree(const char *path)
{
   struct stat st;
   if (rollback_lstat(path, &st) != 0)
      return errno == ENOENT ? 0 : -1;
   if (!S_ISDIR(st.st_mode))
      return unlink(path);

   DIR *d = opendir(path);
   if (!d)
      return -1;
   struct dirent *ent;
   int rc = 0;
   while ((ent = readdir(d)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
         continue;
      char child[SKILL_PATH_MAX];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (remove_tree(child) != 0)
         rc = -1;
   }
   closedir(d);
   return rc == 0 ? rmdir(path) : -1;
}

static int copy_file(const char *src, const char *dst)
{
   FILE *in = fopen(src, "rb");
   if (!in)
      return -1;
   FILE *out = fopen(dst, "wb");
   if (!out)
   {
      fclose(in);
      return -1;
   }
   char buf[8192];
   size_t n;
   int rc = 0;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      if (fwrite(buf, 1, n, out) != n)
      {
         rc = -1;
         break;
      }
   if (ferror(in))
      rc = -1;
   if (fclose(out) != 0)
      rc = -1;
   fclose(in);
   return rc;
}

static int copy_tree(const char *src, const char *dst)
{
   struct stat st;
   if (rollback_lstat(src, &st) != 0)
      return -1;
   if (!S_ISDIR(st.st_mode))
      return S_ISREG(st.st_mode) ? copy_file(src, dst) : -1;

   if (mkdir(dst, 0777) != 0 && errno != EEXIST)
      return -1;
   DIR *d = opendir(src);
   if (!d)
      return -1;
   struct dirent *ent;
   int rc = 0;
   while ((ent = readdir(d)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
          strcmp(ent->d_name, ".snapshots") == 0)
         continue;
      char child_src[SKILL_PATH_MAX], child_dst[SKILL_PATH_MAX];
      snprintf(child_src, sizeof(child_src), "%s/%s", src, ent->d_name);
      snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, ent->d_name);
      if (copy_tree(child_src, child_dst) != 0)
         rc = -1;
   }
   closedir(d);
   return rc;
}

int skill_rollback_snapshot(const char *project_root, const char *snapshot_id, char *errbuf,
                            size_t errbuf_len)
{
   if (!project_root || !project_root[0])
      return rollback_err(errbuf, errbuf_len, "project root is required"), -1;
   if (!snapshot_id_ok(snapshot_id))
      return rollback_err(errbuf, errbuf_len, "invalid snapshot id"), -1;

   char skills_dir[SKILL_PATH_MAX], snapshot_dir[SKILL_PATH_MAX];
   snprintf(skills_dir, sizeof(skills_dir), "%s/.aimee/skills", project_root);
   snprintf(snapshot_dir, sizeof(snapshot_dir), "%s/.snapshots/%s", skills_dir, snapshot_id);

   struct stat st;
   if (rollback_lstat(snapshot_dir, &st) != 0 || !S_ISDIR(st.st_mode))
      return rollback_err(errbuf, errbuf_len, "skill snapshot not found"), -1;
   if (mkdir_p_local(skills_dir) != 0)
      return rollback_err(errbuf, errbuf_len, "failed to prepare skills directory"), -1;

   DIR *d = opendir(skills_dir);
   if (!d)
      return rollback_err(errbuf, errbuf_len, "failed to read skills directory"), -1;
   struct dirent *ent;
   int rc = 0;
   while ((ent = readdir(d)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
          strcmp(ent->d_name, ".snapshots") == 0)
         continue;
      char path[SKILL_PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", skills_dir, ent->d_name);
      if (remove_tree(path) != 0)
         rc = -1;
   }
   closedir(d);
   if (rc != 0)
      return rollback_err(errbuf, errbuf_len, "failed to clear current skills"), -1;

   d = opendir(snapshot_dir);
   if (!d)
      return rollback_err(errbuf, errbuf_len, "failed to read skill snapshot"), -1;
   while ((ent = readdir(d)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
          strcmp(ent->d_name, ".snapshots") == 0)
         continue;
      char src[SKILL_PATH_MAX], dst[SKILL_PATH_MAX];
      snprintf(src, sizeof(src), "%s/%s", snapshot_dir, ent->d_name);
      snprintf(dst, sizeof(dst), "%s/%s", skills_dir, ent->d_name);
      if (copy_tree(src, dst) != 0)
         rc = -1;
   }
   closedir(d);
   if (rc != 0)
      return rollback_err(errbuf, errbuf_len, "failed to restore skill snapshot"), -1;
   return 0;
}
