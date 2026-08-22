#include "client_config.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
      cJSON *profiles = client_config_profile_list();
      if (!profiles)
      {
         fprintf(stderr, "error listing profiles through the server\n");
         return 1;
      }
      if (cJSON_GetArraySize(profiles) == 0)
         printf("(no profiles)\n");
      cJSON *profile = NULL;
      cJSON_ArrayForEach(profile, profiles)
      {
         if (cJSON_IsString(profile))
            printf("%s\n", profile->valuestring);
      }
      cJSON_Delete(profiles);
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
      if (profile_config_operation("profile-create", name) != 0)
      {
         fprintf(stderr, "error creating profile config through the server\n");
         return 1;
      }
      printf("profile: %s\n", name);
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
      int present = client_config_profile_present(name);
      if (present < 0)
      {
         fprintf(stderr, "error reading profile config through the server\n");
         return 1;
      }
      if (!present)
      {
         fprintf(stderr, "error: profile not found: %s\n", name);
         return 1;
      }
      printf("profile: %s\n", name);
      printf("config:  present\n");
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
      int present = client_config_profile_present(name);
      if (present < 0)
      {
         fprintf(stderr, "error reading profile config through the server\n");
         return 1;
      }
      if (!present)
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
      if (profile_config_operation("profile-delete", name) != 0)
      {
         fprintf(stderr, "error deleting profile through the server\n");
         return 1;
      }
      printf("deleted: %s\n", name);
      return 0;
   }

   fprintf(stderr, "Usage: aimee profile <create|list|show|delete|current>\n");
   return 1;
}
