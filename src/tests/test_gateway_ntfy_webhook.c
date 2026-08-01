/* test_gateway_ntfy_webhook.c: unit tests for ntfy and webhook adapters. */
#include "gateway/platform_ntfy.h"
#include "gateway/platform_webhook.h"
#include "gateway/gateway_platform.h"
#include "runtime_secret.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(name) printf("  PASS: %s\n", name)

static void test_ntfy_check_config_always_passes(void)
{
   platform_adapter_t *a = ntfy_adapter_get();
   assert(a != NULL);
   char err[128] = {0};
   /* ntfy has a default base URL — check_config always returns 0. */
   int rc = a->check_config(a, err, sizeof(err));
   assert(rc == 0);
   PASS("ntfy_check_config_always_passes");
}

static void test_ntfy_adapter_name(void)
{
   platform_adapter_t *a = ntfy_adapter_get();
   assert(a != NULL);
   assert(strcmp(a->name, "ntfy") == 0);
   PASS("ntfy_adapter_name");
}

static void test_ntfy_send_text_null_target_fails(void)
{
   platform_adapter_t *a = ntfy_adapter_get();
   assert(a != NULL);
   int rc = a->send_text(a, NULL, "hello");
   assert(rc == -1);
   PASS("ntfy_send_text_null_target_fails");
}

static void test_webhook_check_config_fails_without_secret(void)
{
   /* Ensure the env vars are not set. */
   unsetenv("AIMEE_GATEWAY_WEBHOOK_SECRET");
   runtime_secret_remove("AIMEE_GATEWAY_WEBHOOK_SECRET");
   unsetenv("AIMEE_GATEWAY_WEBHOOK_INSECURE");

   platform_adapter_t *a = webhook_adapter_get();
   assert(a != NULL);
   char err[256] = {0};
   int rc = a->check_config(a, err, sizeof(err));
   assert(rc == -1);
   assert(err[0] != '\0');
   PASS("webhook_check_config_fails_without_secret");
}

static void test_webhook_check_config_passes_when_insecure(void)
{
   unsetenv("AIMEE_GATEWAY_WEBHOOK_SECRET");
   setenv("AIMEE_GATEWAY_WEBHOOK_INSECURE", "true", 1);

   platform_adapter_t *a = webhook_adapter_get();
   assert(a != NULL);
   char err[256] = {0};
   int rc = a->check_config(a, err, sizeof(err));
   assert(rc == 0);

   unsetenv("AIMEE_GATEWAY_WEBHOOK_INSECURE");
   PASS("webhook_check_config_passes_when_insecure");
}

static void test_webhook_check_config_passes_with_secret(void)
{
   assert(runtime_secret_store("AIMEE_GATEWAY_WEBHOOK_SECRET", "supersecret") == 0);
   unsetenv("AIMEE_GATEWAY_WEBHOOK_INSECURE");

   platform_adapter_t *a = webhook_adapter_get();
   assert(a != NULL);
   char err[256] = {0};
   int rc = a->check_config(a, err, sizeof(err));
   /* With a secret set and INSECURE unset, HMAC is required.
    * Phase 1 stub denies because OpenSSL is not linked — check_config still passes
    * (it only validates config presence, not HMAC capability). */
   assert(rc == 0);

   runtime_secret_remove("AIMEE_GATEWAY_WEBHOOK_SECRET");
   PASS("webhook_check_config_passes_with_secret");
}

static void test_webhook_adapter_name(void)
{
   platform_adapter_t *a = webhook_adapter_get();
   assert(a != NULL);
   assert(strcmp(a->name, "webhook") == 0);
   PASS("webhook_adapter_name");
}

int main(void)
{
   printf("test_gateway_ntfy_webhook\n");
   test_ntfy_check_config_always_passes();
   test_ntfy_adapter_name();
   test_ntfy_send_text_null_target_fails();
   test_webhook_check_config_fails_without_secret();
   test_webhook_check_config_passes_when_insecure();
   test_webhook_check_config_passes_with_secret();
   test_webhook_adapter_name();
   printf("All tests passed.\n");
   return 0;
}
