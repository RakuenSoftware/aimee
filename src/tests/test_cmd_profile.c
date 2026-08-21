/* test_cmd_profile.c: profile commands are thin event-bus clients. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "client_config.h"
#include "cJSON.h"

int cmd_profile_run(int argc, char **argv);

static int g_profile_created;

static cJSON *profile_operation(const char *operation, const cJSON *value)
{
   const char *name =
       value ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, "name")) : NULL;
   cJSON *response = cJSON_CreateObject();
   /* Production /v1 envelopes report status:"ok". Keep this test on the real
    * wire shape so a client cannot accidentally accept only an in-process
    * boolean test-double convention. */
   cJSON_AddStringToObject(response, "status", "ok");
   if (!strcmp(operation, "profile-create") && name && !strcmp(name, "coder"))
      g_profile_created = 1;
   if (!strcmp(operation, "profile-delete") && name && !strcmp(name, "coder"))
      g_profile_created = 0;
   if (!strcmp(operation, "profile-present"))
      cJSON_AddBoolToObject(response, "present",
                            g_profile_created && name && !strcmp(name, "coder"));
   if (!strcmp(operation, "profile-list"))
   {
      cJSON *profiles = cJSON_AddArrayToObject(response, "profiles");
      if (g_profile_created)
         cJSON_AddItemToArray(profiles, cJSON_CreateString("coder"));
   }
   return response;
}

static void test_create_show_delete_profile(void)
{
   client_config_set_operation_provider(profile_operation);
   char root[256];
   snprintf(root, sizeof(root), "/tmp/aimee-cmd-profile-test-%ld", (long)getpid());
   setenv("AIMEE_HOME", root, 1);
   unsetenv("AIMEE_PROFILE");

   char *create_args[] = {(char *)"create", (char *)"coder"};
   assert(cmd_profile_run(2, create_args) == 0);
   assert(access(root, F_OK) != 0);

   char *list_args[] = {(char *)"list"};
   assert(cmd_profile_run(1, list_args) == 0);

   char *show_args[] = {(char *)"show", (char *)"coder"};
   assert(cmd_profile_run(2, show_args) == 0);

   char *delete_args[] = {(char *)"delete", (char *)"coder", (char *)"--force"};
   assert(cmd_profile_run(3, delete_args) == 0);
   assert(g_profile_created == 0);
   assert(cmd_profile_run(2, show_args) != 0);
   assert(cmd_profile_run(1, list_args) == 0);
   assert(access(root, F_OK) != 0);
   printf("  PASS: test_create_show_delete_profile\n");
}

static void test_invalid_profile_name_rejected(void)
{
   char root[256];
   snprintf(root, sizeof(root), "/tmp/aimee-cmd-profile-invalid-%ld", (long)getpid());
   setenv("AIMEE_HOME", root, 1);
   unsetenv("AIMEE_PROFILE");

   char *show_args[] = {(char *)"show", (char *)"../bad"};
   assert(cmd_profile_run(2, show_args) != 0);
   assert(access(root, F_OK) != 0);
   printf("  PASS: test_invalid_profile_name_rejected\n");
}

int main(void)
{
   printf("cmd_profile:\n");
   test_create_show_delete_profile();
   test_invalid_profile_name_rejected();
   printf("ok\n");
   return 0;
}
