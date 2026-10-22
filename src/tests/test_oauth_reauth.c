/* test_oauth_reauth.c — D6 REAUTH_REQUIRED credential state.
 *
 * Proves the marker the codex refresh path sets on an IdP rejection round-trips
 * through the vault and is cleared by the next successful token store (a fresh
 * operator re-auth), so a recovered codex credential no longer reports as
 * needing re-auth. */
#include "oauth_flow.h"
#include "cJSON.h"
#include "vault_service.h"
#include "vault_server_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

/* Mirrors OAUTH_VCRED_REAUTH in oauth_tokens.c (the marker cred slot). */
#define REAUTH_VCRED "oauth_reauth_required"

static int allocation_index, allocation_fail_at;
static void *oauth_test_allocate(size_t size)
{
   return ++allocation_index == allocation_fail_at ? NULL : malloc(size);
}
typedef struct
{
   const oauth_token_response_t *first, *next;
   atomic_int start;
   int failed;
} oauth_rotation_writer_t;
static void *oauth_rotation_writer(void *context)
{
   oauth_rotation_writer_t *writer = context;
   while (!atomic_load(&writer->start))
      usleep(1000);
   for (int i = 0; i < 100; i++)
      if (oauth_token_store("atomic-fixture", i & 1 ? writer->first : writer->next))
      {
         writer->failed = 1;
         break;
      }
   return NULL;
}
static void test_atomic_source_and_rotation(void)
{
   oauth_token_response_t first = {0}, next = {0};
   snprintf(first.access_token, sizeof first.access_token, "atomic-access-first");
   snprintf(first.refresh_token, sizeof first.refresh_token, "atomic-refresh-first");
   first.expires_at = (long)time(NULL) + 3600;
   snprintf(next.access_token, sizeof next.access_token, "atomic-access-next");
   snprintf(next.refresh_token, sizeof next.refresh_token, "atomic-refresh-next");
   next.expires_at = first.expires_at + 3600;
   char source_first[65], source_next[65];
   memset(source_first, 'a', 64);
   source_first[64] = 0;
   memset(source_next, 'b', 64);
   source_next[64] = 0;
   assert(!oauth_token_store_source("atomic-fixture", &first, source_first));
   cJSON_Hooks hooks = {.malloc_fn = oauth_test_allocate, .free_fn = free};
   allocation_index = allocation_fail_at = 0;
   cJSON_InitHooks(&hooks);
   int rc = oauth_token_store_source("atomic-fixture", &next, source_next);
   int allocations = allocation_index;
   cJSON_InitHooks(NULL);
   assert(!rc && allocations > 4);
   const int failure_points[] = {1, allocations / 2, allocations - 2, allocations - 1, allocations};
   char access[64], refresh[64], expiry[32], rejection[8], source[65];
   vault_store_read_t reads[] = {{"oauth_access_token", access, sizeof access, 0},
                                 {"oauth_refresh_token", refresh, sizeof refresh, 0},
                                 {"oauth_expires_at", expiry, sizeof expiry, 0},
                                 {REAUTH_VCRED, rejection, sizeof rejection, 0},
                                 {OAUTH_SOURCE_CREDENTIAL, source, sizeof source, 0}};
   int failures = 0;
   for (size_t i = 0; i < sizeof failure_points / sizeof *failure_points; i++)
   {
      assert(!oauth_token_store_source("atomic-fixture", &first, source_first));
      assert(vault_service_set_server("atomic-fixture", REAUTH_VCRED, "1") == VAULT_OK);
      allocation_index = 0;
      allocation_fail_at = failure_points[i];
      cJSON_InitHooks(&hooks);
      rc = oauth_token_store_source("atomic-fixture", &next, source_next);
      cJSON_InitHooks(NULL);
      assert(vault_service_read_server_snapshot("atomic-fixture", reads, 5) == VAULT_OK);
      const oauth_token_response_t *expected = rc ? &first : &next;
      assert(!strcmp(access, expected->access_token) && !strcmp(refresh, expected->refresh_token));
      assert(atol(expiry) == expected->expires_at);
      assert(!strcmp(source, rc ? source_first : source_next));
      assert(!strcmp(rejection, rc ? "1" : "0"));
      if (rc)
         failures++;
   }
   assert(failures > 0);
   assert(!oauth_token_store_source("atomic-fixture", &first, source_first));
   next.refresh_token[0] = 0;
   assert(oauth_token_store_source("atomic-fixture", &next, source_next) == -1);
   assert(!oauth_token_store("atomic-fixture", &next));
   assert(vault_service_read_server_snapshot("atomic-fixture", reads, 5) == VAULT_OK);
   assert(!strcmp(access, next.access_token) && !strcmp(refresh, first.refresh_token));
   assert(!strcmp(source, source_first));

   /* Readers may see either whole generation; an expired access token must
    * never acquire the other generation's future expiry. */
   next.expires_at = (long)time(NULL) - 10;
   assert(!oauth_token_store("atomic-fixture", &first));
   /* Server credential custody deliberately refuses inherited post-fork state;
    * model concurrent request workers without weakening that child boundary. */
   oauth_rotation_writer_t state = {.first = &first, .next = &next};
   pthread_t writer;
   assert(!pthread_create(&writer, NULL, oauth_rotation_writer, &state));
   atomic_store(&state.start, 1);
   for (int i = 0; i < 300; i++)
   {
      rc = oauth_token_load("atomic-fixture", access, sizeof access);
      assert(rc ? !access[0] : !strcmp(access, first.access_token));
   }
   assert(!pthread_join(writer, NULL) && !state.failed);
   printf("PASS: atomic OAuth source/refresh/expiry/rejection, %d injected failures and concurrent "
          "validity\n",
          failures);
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-oauthreauth-test-%d", (int)getpid());
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(cmd) == 0);
   setenv("AIMEE_HOME", home, 1);
   vault_server_key_reset_for_test();

   /* Fresh credential: not flagged. */
   assert(oauth_token_reauth_required("codex") == 0);

   /* Simulate the refresh-rejection path setting the marker. */
   assert(vault_service_set_server("codex", REAUTH_VCRED, "1") == VAULT_OK);
   assert(oauth_token_reauth_required("codex") == 1 && "marker should read back set");

   /* A successful token store (operator re-auth) clears the marker. */
   oauth_token_response_t resp;
   memset(&resp, 0, sizeof(resp));
   snprintf(resp.access_token, sizeof(resp.access_token), "tok-after-reauth");
   snprintf(resp.refresh_token, sizeof(resp.refresh_token), "rt-after-reauth");
   resp.expires_at = time(NULL) + 3600;
   assert(oauth_token_store("codex", &resp) == 0);
   assert(oauth_token_reauth_required("codex") == 0 &&
          "a successful token store must clear REAUTH_REQUIRED");

   /* And the fresh token is now usable (no refresh needed). */
   char buf[256] = "";
   assert(oauth_token_get("codex", NULL, NULL, 0, buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "tok-after-reauth") == 0 && "stored access token should load");

   /* The high-level getter must honor the same validity contract as load. */
   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   snprintf(buf, sizeof buf, "%s", "stale caller buffer");
   assert(oauth_token_get("codex", NULL, NULL, 0, buf, sizeof buf) == -1 && !buf[0]);
   resp.expires_at = time(NULL) + 3600;
   assert(!oauth_token_store("codex", &resp));
   assert(vault_service_set_server("codex", REAUTH_VCRED, "1") == VAULT_OK);
   assert(oauth_token_get("codex", NULL, NULL, 0, buf, sizeof buf) == -1 && !buf[0]);

   /* Exercise the real refresh transport with a private fake curl. No network
    * or operator credential is involved; the real Vault stores each result. */
   char curl_path[320], test_path[8192];
   const char *inherited_path = getenv("PATH");
   char *saved_path = inherited_path ? strdup(inherited_path) : NULL;
   snprintf(curl_path, sizeof curl_path, "%s/curl", home);
   FILE *curl = fopen(curl_path, "w");
   assert(curl);
   assert(fputs("#!/bin/sh\n"
                "printf '%s\\n' \"$@\" >\"$AIMEE_TEST_OAUTH_ARGS\"\n"
                "cat >\"$AIMEE_TEST_OAUTH_BODY\"\n"
                "printf '%s' \"$AIMEE_TEST_OAUTH_RESPONSE\"\n",
                curl) >= 0);
   int close_rc = fclose(curl);
   assert(!close_rc);
   assert(!chmod(curl_path, 0700));
   assert(snprintf(test_path, sizeof test_path, "%s:%s", home,
                   saved_path ? saved_path : "/usr/bin:/bin") < (int)sizeof test_path);
   assert(!setenv("PATH", test_path, 1));
   char args_path[320], body_path[320];
   snprintf(args_path, sizeof args_path, "%s/args", home);
   snprintf(body_path, sizeof body_path, "%s/body", home);
   assert(!setenv("AIMEE_TEST_OAUTH_ARGS", args_path, 1));
   assert(!setenv("AIMEE_TEST_OAUTH_BODY", body_path, 1));

   resp.expires_at = time(NULL) + 300;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE", "{\"error\":\"temporarily_unavailable\"}\n503", 1));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   assert(!strcmp(buf, "tok-after-reauth"));
   assert(!oauth_token_reauth_required("codex"));

   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   assert(oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                          sizeof buf) == -1 &&
          !buf[0]);

   resp.expires_at = time(NULL) + 300;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE", "{\"error\":\"invalid_grant\"}\n400", 1));
   assert(oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                          sizeof buf) == -1 &&
          !buf[0]);
   assert(oauth_token_reauth_required("codex"));

   /* Successful refresh replaces an expired access token and clears the marker
    * only through a successful store (a new login first clears rejection). */
   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE",
                  "{\"access_token\":\"refreshed-fixture\",\"expires_in\":3600}\n200", 1));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   assert(!strcmp(buf, "refreshed-fixture"));
   assert(!oauth_token_reauth_required("codex"));

   /* Token-looking 5xx responses and descriptions mentioning invalid_grant
    * cannot rotate credentials or falsely latch a reauthentication marker. */
   resp.expires_at = time(NULL) + 300;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE", "{\"access_token\":\"must-not-store\"}\n503", 1));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   assert(!strcmp(buf, "tok-after-reauth"));
   assert(!setenv(
       "AIMEE_TEST_OAUTH_RESPONSE",
       "{\"error\":\"temporarily_unavailable\",\"description\":\"upstream invalid_grant\"}\n400",
       1));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   assert(!oauth_token_reauth_required("codex"));

   /* Claude's refresh profile is JSON, with the exact granted scope/client and
    * full-length refresh token. Credentials must stay out of process arguments. */
   memset(resp.refresh_token, '+', 900);
   resp.refresh_token[900] = 0;
   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE",
                  "{\"access_token\":\"json-refreshed\",\"expires_in\":3600}\n200", 1));
   const oauth_refresh_profile_t profile = {.client_id = "fixture-client",
                                            .token_endpoint =
                                                "https://fixture.invalid/token?literal='quote'",
                                            .scope = "user:profile user:inference",
                                            .format = OAUTH_REFRESH_JSON};
   assert(!oauth_token_get_profile("codex", &profile, 600, buf, sizeof buf));
   assert(!strcmp(buf, "json-refreshed"));
   char captured[8192];
   FILE *capture = fopen(body_path, "r");
   assert(capture);
   size_t captured_len = fread(captured, 1, sizeof captured - 1, capture);
   captured[captured_len] = 0;
   close_rc = fclose(capture);
   assert(!close_rc);
   cJSON *json = cJSON_Parse(captured);
   assert(cJSON_IsObject(json));
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "refresh_token")->valuestring,
                  resp.refresh_token));
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "client_id")->valuestring,
                  profile.client_id));
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "scope")->valuestring, profile.scope));
   cJSON_Delete(json);
   capture = fopen(args_path, "r");
   assert(capture);
   captured_len = fread(captured, 1, sizeof captured - 1, capture);
   captured[captured_len] = 0;
   close_rc = fclose(capture);
   assert(!close_rc);
   assert(strstr(captured, "application/json") && strstr(captured, "--data-binary\n@-"));
   assert(strstr(captured, profile.token_endpoint));
   assert(strstr(captured, "--max-time\n30"));
   assert(!strstr(captured, resp.refresh_token));

   /* The legacy form profile must encode the whole token, not truncate at the
    * old 512-byte refresh/encoding buffers. */
   assert(!oauth_token_store("codex", &resp));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   capture = fopen(body_path, "r");
   assert(capture);
   captured_len = fread(captured, 1, sizeof captured - 1, capture);
   captured[captured_len] = 0;
   close_rc = fclose(capture);
   assert(!close_rc);
   const char *encoded = strstr(captured, "refresh_token=");
   assert(encoded);
   encoded += strlen("refresh_token=");
   for (int i = 0; i < 900; i++)
      assert(!strncmp(encoded + 3 * i, "%2B", 3));
   assert(!strcmp(encoded + 2700, "&client_id=fixture-client"));

   resp.expires_at = 0;
   assert(!oauth_token_store("codex", &resp));
   assert(!oauth_token_get("codex", NULL, NULL, 0, buf, sizeof buf));
   assert(!strcmp(buf, "tok-after-reauth"));
   assert(oauth_token_get(NULL, NULL, NULL, 0, buf, sizeof buf) == -1 && !buf[0]);

   if (saved_path)
      assert(!setenv("PATH", saved_path, 1));
   else
      assert(!unsetenv("PATH"));
   free(saved_path);
   assert(!unsetenv("AIMEE_TEST_OAUTH_RESPONSE"));
   assert(!unsetenv("AIMEE_TEST_OAUTH_ARGS"));
   assert(!unsetenv("AIMEE_TEST_OAUTH_BODY"));

   test_atomic_source_and_rotation();
   snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
   (void)system(cmd);
   printf("PASS: OAuth token validity, refresh fallback and REAUTH_REQUIRED (D6)\n");
   return 0;
}
