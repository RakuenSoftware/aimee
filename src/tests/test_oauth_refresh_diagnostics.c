/* Refresh diagnostics must classify failures without recording any credential,
 * endpoint, response body, or curl stderr. Exercise the real curl subprocess
 * boundary with a local executable; no network or real credentials are used. */
#include "oauth_flow.h"
#include "vault_service.h"
#include "vault_server_key.h"
#include "log.h"
#include "platform_test_util.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char diagnostic[8192];
static int fail_store;
static void refresh_test_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   assert(!strcmp(module, "oauth_flow"));
   size_t used = strlen(diagnostic);
   va_list args;
   va_start(args, format);
   int n = vsnprintf(diagnostic + used, sizeof diagnostic - used, format, args);
   va_end(args);
   assert(n >= 0 && (size_t)n < sizeof diagnostic - used);
}
static vault_status_t refresh_test_store(const char *client, const vault_store_write_t *writes,
                                         size_t count)
{
   return fail_store ? VAULT_ERR_IO : vault_service_write_server_batch(client, writes, count);
}

/* Scoped wrappers capture only this production unit's diagnostics and inject a
 * failed persistence result. The normal store and subprocess remain real. */
#define aimee_log                        refresh_test_log
#define vault_service_write_server_batch refresh_test_store
#include "../server/oauth_tokens.c"
#undef vault_service_write_server_batch
#undef aimee_log

static void no_secret_diagnostic(void)
{
   const char *canaries[] = {"SECRET_REFRESH", "SECRET_ACCESS", "SECRET_ENDPOINT",
                             "SECRET_CLIENT",  "SECRET_BODY",   "SECRET_STDERR"};
   for (size_t i = 0; i < sizeof canaries / sizeof *canaries; i++)
      assert(!strstr(diagnostic, canaries[i]));
}
static void run_case(const char *response, const char *exit_code, const char *category,
                     const char *numeric)
{
   diagnostic[0] = 0;
   assert(!setenv("REFRESH_TEST_RESPONSE", response, 1));
   assert(!setenv("REFRESH_TEST_EXIT", exit_code, 1));
   assert(oauth_token_refresh("diagnostic-fixture", "SECRET_CLIENT",
                              "https://SECRET_ENDPOINT.invalid/token") == -1);
   assert(strstr(diagnostic, category));
   if (numeric)
      assert(strstr(diagnostic, numeric));
   no_secret_diagnostic();
   assert(!oauth_token_reauth_required("diagnostic-fixture"));
}
int main(void)
{
   char home[512], curl_path[640], path[4096];
   snprintf(home, sizeof home, "%s/aimee-refresh-diagnostic-XXXXXX", platform_tmpdir());
   assert(mkdtemp(home));
   assert(!setenv("AIMEE_HOME", home, 1));
   vault_server_key_reset_for_test();
   oauth_token_response_t seed = {0};
   snprintf(seed.access_token, sizeof seed.access_token, "SECRET_ACCESS-original");
   snprintf(seed.refresh_token, sizeof seed.refresh_token, "SECRET_REFRESH");
   seed.expires_at = (long)time(NULL) + 3600;
   assert(!oauth_token_store("diagnostic-fixture", &seed));
   snprintf(curl_path, sizeof curl_path, "%s/curl", home);
   FILE *file = fopen(curl_path, "w");
   assert(file);
   assert(fputs("#!/bin/sh\ncat >/dev/null\nprintf '%s' \"$REFRESH_TEST_RESPONSE\"\n"
                "printf '%s' SECRET_STDERR >&2\nexit \"$REFRESH_TEST_EXIT\"\n",
                file) >= 0);
   assert(!fclose(file) && !chmod(curl_path, 0700));
   const char *old_path = getenv("PATH");
   assert(snprintf(path, sizeof path, "%s:%s", home, old_path ? old_path : "/usr/bin:/bin") <
          (int)sizeof path);
   assert(!setenv("PATH", path, 1));

   run_case("SECRET_BODY", "28", "category=transport", "curl_exit=28");
   /* Stderr precedes the final status in a genuine curl response. Put the
    * canary there too: neither content is diagnostic authority. */
   assert(!setenv("REFRESH_TEST_EXIT", "0", 1));
   file = fopen(curl_path, "w");
   assert(file && fputs("#!/bin/sh\ncat >/dev/null\nprintf '%s' SECRET_STDERR >&2\n"
                        "printf '%s' \"$REFRESH_TEST_RESPONSE\"\n"
                        "exit \"$REFRESH_TEST_EXIT\"\n",
                        file) >= 0);
   assert(!fclose(file));
   run_case("{\"description\":\"SECRET_BODY\"}\n503", "0", "category=http", "http_status=503");
   run_case("{\"description\":\"SECRET_BODY\"}\n200", "0", "category=response_parse", NULL);

   /* A malformed status must not be logged as a numeric HTTP status. Existing
    * refresh behavior is otherwise unchanged. */
   run_case("SECRET_BODY\nSECRET_BODY", "0", "category=http", NULL);
   assert(!strstr(diagnostic, "http_status="));
   fail_store = 1;
   /* Remove stderr for the valid token response/persistence failure case. */
   file = fopen(curl_path, "w");
   assert(file && fputs("#!/bin/sh\ncat >/dev/null\nprintf '%s' \"$REFRESH_TEST_RESPONSE\"\n"
                        "exit \"$REFRESH_TEST_EXIT\"\n",
                        file) >= 0);
   assert(!fclose(file));
   run_case("{\"access_token\":\"SECRET_ACCESS-new\",\"expires_in\":3600}\n200", "0",
            "category=credential_store", NULL);
   fail_store = 0;
   char access[128];
   assert(!oauth_token_load("diagnostic-fixture", access, sizeof access));
   assert(!strcmp(access, seed.access_token));
   diagnostic[0] = 0;
   assert(!oauth_token_refresh("diagnostic-fixture", "SECRET_CLIENT",
                               "https://SECRET_ENDPOINT.invalid/token"));
   assert(!diagnostic[0]);
   assert(!oauth_token_load("diagnostic-fixture", access, sizeof access));
   assert(!strcmp(access, "SECRET_ACCESS-new"));
   diagnostic[0] = 0;
   assert(!setenv("REFRESH_TEST_RESPONSE",
                  "{\"error\":\"invalid_grant\",\"description\":\"SECRET_BODY\"}\n400", 1));
   assert(oauth_token_refresh("diagnostic-fixture", "SECRET_CLIENT",
                              "https://SECRET_ENDPOINT.invalid/token") == OAUTH_REFRESH_REAUTH);
   assert(strstr(diagnostic, "category=credential_rejected") &&
          strstr(diagnostic, "http_status=400"));
   no_secret_diagnostic();
   assert(oauth_token_reauth_required("diagnostic-fixture"));
   assert(!oauth_token_store("diagnostic-fixture", &seed));
   diagnostic[0] = 0;
   assert(!oauth_token_remove("diagnostic-fixture"));
   run_case("SECRET_BODY", "0", "category=load_refresh", NULL);
   puts("OAuth refresh static failure categories, bounded codes and secret exclusion: ok");
   return 0;
}
