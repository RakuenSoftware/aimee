#include "aimee_home.h"
#include "client_config.h"
#include "platform_path.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

static int profile_name_valid(const char *name)
{
   if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      return 0;
   if (name[0] == '.')
      return 0;
   for (const char *p = name; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c) || c == '-' || c == '_' || c == '.')
         continue;
      return 0;
   }
   return 1;
}

static int path_join(char *out, size_t outsz, const char *dir, const char *leaf)
{
   int n = snprintf(out, outsz, "%s/%s", dir ? dir : "", leaf ? leaf : "");
   return n > 0 && (size_t)n < outsz ? 0 : -1;
}

static int profile_config_operation(const char *operation, const char *name)
{
   cJSON *value = cJSON_CreateObject();
   if (!value || !cJSON_AddStringToObject(value, "name", name))
   {
      cJSON_Delete(value);
      return -1;
   }
   return client_config_operation(operation, value);
}

static int is_dir(const char *path)
{
   struct stat st;
   return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int remove_tree(const char *path)
{
   DIR *d = opendir(path);
   if (!d)
      return -1;

   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;

      char child[4096];
      if (path_join(child, sizeof(child), path, e->d_name) != 0)
      {
         closedir(d);
         return -1;
      }

      struct stat st;
      if (lstat(child, &st) != 0)
      {
         closedir(d);
         return -1;
      }
      if (S_ISDIR(st.st_mode))
      {
         if (remove_tree(child) != 0)
         {
            closedir(d);
            return -1;
         }
      }
      else if (unlink(child) != 0)
      {
         closedir(d);
         return -1;
      }
   }
   closedir(d);
   return rmdir(path);
}

int cmd_profile_run(int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee profile <create|list|show|delete|current>\n");
      return 1;
   }

   const char *sub = argv[0];

   if (strcmp(sub, "current") == 0)
   {
      const char *p = getenv("AIMEE_PROFILE");
      printf("%s\n", (p && p[0]) ? p : "default");
      return 0;
   }

   if (strcmp(sub, "list") == 0)
   {
      const char *base = aimee_profiles_dir();
      if (!base)
         return 1;
      DIR *d = opendir(base);
      if (!d)
      {
         printf("(no profiles)\n");
         return 0;
      }
      struct dirent *e;
      while ((e = readdir(d)) != NULL)
      {
         if (e->d_name[0] == '.')
            continue;
         char path[4096];
         if (path_join(path, sizeof(path), base, e->d_name) == 0 && is_dir(path))
            printf("%s\n", e->d_name);
      }
      closedir(d);
      return 0;
   }

   if (strcmp(sub, "create") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee profile create <name>\n");
         return 1;
      }
      const char *name = argv[1];
      if (!profile_name_valid(name))
      {
         fprintf(stderr, "error: invalid profile name: %s\n", name);
         return 1;
      }
      const char *base = aimee_profiles_dir();
      if (!base)
         return 1;
      if (platform_mkdir_p(base, 0700) != 0)
      {
         fprintf(stderr, "error creating profiles directory: %s\n", strerror(errno));
         return 1;
      }
      char dir[4096];
      if (path_join(dir, sizeof(dir), base, name) != 0)
         return 1;
      if (platform_mkdir_p(dir, 0700) != 0)
      {
         fprintf(stderr, "error creating profile: %s\n", strerror(errno));
         return 1;
      }
      if (profile_config_operation("profile-create", name) != 0)
      {
         fprintf(stderr, "error creating profile config through the server\n");
         return 1;
      }
      printf("profile: %s\n", name);
      printf("path:    %s\n", dir);
      printf("config:  present\n");
      return 0;
   }

   if (strcmp(sub, "show") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee profile show <name>\n");
         return 1;
      }
      const char *name = argv[1];
      if (!profile_name_valid(name))
      {
         fprintf(stderr, "error: invalid profile name: %s\n", name);
         return 1;
      }
      const char *base = aimee_profiles_dir();
      if (!base)
         return 1;
      char dir[4096];
      if (path_join(dir, sizeof(dir), base, name) != 0)
         return 1;
      if (!is_dir(dir))
      {
         fprintf(stderr, "error: profile not found: %s\n", name);
         return 1;
      }
      printf("profile: %s\n", name);
      printf("path:    %s\n", dir);
      int present = client_config_profile_present(name);
      if (present < 0)
      {
         fprintf(stderr, "error reading profile config through the server\n");
         return 1;
      }
      printf("config:  %s\n", present ? "present" : "absent");
      return 0;
   }

   if (strcmp(sub, "delete") == 0)
   {
      int force = 0;
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee profile delete <name> [--force]\n");
         return 1;
      }
      const char *name = argv[1];
      for (int i = 2; i < argc; i++)
      {
         if (strcmp(argv[i], "--force") == 0)
            force = 1;
         else
         {
            fprintf(stderr, "Usage: aimee profile delete <name> [--force]\n");
            return 1;
         }
      }
      if (!profile_name_valid(name))
      {
         fprintf(stderr, "error: invalid profile name: %s\n", name);
         return 1;
      }
      const char *active = getenv("AIMEE_PROFILE");
      if (active && strcmp(active, name) == 0)
      {
         fprintf(stderr, "error: cannot delete the active profile\n");
         return 1;
      }
      const char *base = aimee_profiles_dir();
      if (!base)
         return 1;
      char dir[4096];
      if (path_join(dir, sizeof(dir), base, name) != 0)
         return 1;
      if (!is_dir(dir))
      {
         fprintf(stderr, "error: profile not found: %s\n", name);
         return 1;
      }
      if (!force)
      {
         if (!isatty(STDIN_FILENO))
         {
            fprintf(stderr, "error: refusing non-interactive delete without --force\n");
            return 1;
         }
         char answer[256];
         fprintf(stderr, "Delete profile '%s'? Type the profile name to confirm: ", name);
         if (!fgets(answer, sizeof(answer), stdin))
            return 1;
         answer[strcspn(answer, "\r\n")] = '\0';
         if (strcmp(answer, name) != 0)
         {
            fprintf(stderr, "delete cancelled\n");
            return 1;
         }
      }
      if (remove_tree(dir) != 0)
      {
         fprintf(stderr, "error deleting profile: %s\n", strerror(errno));
         return 1;
      }
      printf("deleted: %s\n", name);
      return 0;
   }

   fprintf(stderr, "Usage: aimee profile <create|list|show|delete|current>\n");
   return 1;
}
