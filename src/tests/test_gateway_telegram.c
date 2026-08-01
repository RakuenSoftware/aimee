/* test_gateway_telegram.c: unit tests for the Telegram platform adapter. */
#include "gateway/platform_telegram.h"
#include "gateway/gateway_platform.h"
#include "runtime_secret.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(name) printf("  PASS: %s\n", name)

static void test_check_config_fails_without_token(void)
{
   unsetenv("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   runtime_secret_remove("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   unsetenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS");

   platform_adapter_t *a = telegram_adapter_get();
   assert(a != NULL);
   char err[256] = {0};
   int rc = a->check_config(a, err, sizeof(err));
   assert(rc == -1);
   assert(err[0] != '\0');
   PASS("check_config_fails_without_token");
}

static void test_check_config_fails_without_allowed_users(void)
{
   assert(runtime_secret_store("AIMEE_GATEWAY_TELEGRAM_TOKEN", "test-bot-token") == 0);
   unsetenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS");

   platform_adapter_t *a = telegram_adapter_get();
   assert(a != NULL);
   char err[256] = {0};
   int rc = a->check_config(a, err, sizeof(err));
   assert(rc == -1);

   runtime_secret_remove("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   PASS("check_config_fails_without_allowed_users");
}

static void test_check_config_passes_when_both_set(void)
{
   assert(runtime_secret_store("AIMEE_GATEWAY_TELEGRAM_TOKEN", "test-bot-token") == 0);
   setenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS", "111111,222222", 1);

   platform_adapter_t *a = telegram_adapter_get();
   assert(a != NULL);
   char err[256] = {0};
   int rc = a->check_config(a, err, sizeof(err));
   assert(rc == 0);

   runtime_secret_remove("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   unsetenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS");
   PASS("check_config_passes_when_both_set");
}

static void test_authorize_user_allows_listed_user(void)
{
   assert(runtime_secret_store("AIMEE_GATEWAY_TELEGRAM_TOKEN", "tok") == 0);
   setenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS", "111111,222222,333333", 1);

   /* Run check_config first to populate internal state. */
   platform_adapter_t *a = telegram_adapter_get();
   assert(a != NULL);
   a->check_config(a, NULL, 0);

   int rc = a->authorize_user(a, "telegram", "chat1", "222222");
   assert(rc == 0);

   runtime_secret_remove("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   unsetenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS");
   PASS("authorize_user_allows_listed_user");
}

static void test_authorize_user_denies_unlisted_user(void)
{
   assert(runtime_secret_store("AIMEE_GATEWAY_TELEGRAM_TOKEN", "tok") == 0);
   setenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS", "111111,222222", 1);

   platform_adapter_t *a = telegram_adapter_get();
   assert(a != NULL);
   a->check_config(a, NULL, 0);

   int rc = a->authorize_user(a, "telegram", "chat1", "999999");
   assert(rc == -1);

   runtime_secret_remove("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   unsetenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS");
   PASS("authorize_user_denies_unlisted_user");
}

static void test_authorize_user_denies_empty_allowlist(void)
{
   assert(runtime_secret_store("AIMEE_GATEWAY_TELEGRAM_TOKEN", "tok") == 0);
   /* Set ALLOWED_USERS to a value so check_config passes, then clear it. */
   setenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS", "dummy", 1);
   platform_adapter_t *a = telegram_adapter_get();
   a->check_config(a, NULL, 0);

   /* Now clear it and test directly — authorize_user re-reads the env. */
   setenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS", "", 1);
   int rc = a->authorize_user(a, "telegram", "chat1", "111111");
   assert(rc == -1);

   runtime_secret_remove("AIMEE_GATEWAY_TELEGRAM_TOKEN");
   unsetenv("AIMEE_GATEWAY_TELEGRAM_ALLOWED_USERS");
   PASS("authorize_user_denies_empty_allowlist");
}

int main(void)
{
   printf("test_gateway_telegram\n");
   test_check_config_fails_without_token();
   test_check_config_fails_without_allowed_users();
   test_check_config_passes_when_both_set();
   test_authorize_user_allows_listed_user();
   test_authorize_user_denies_unlisted_user();
   test_authorize_user_denies_empty_allowlist();
   printf("All tests passed.\n");
   return 0;
}
