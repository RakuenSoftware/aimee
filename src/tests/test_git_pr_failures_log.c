/* test_git_pr_failures_log.c — what `git pr failures` hands back from a job log.
 *
 * Pins the two ways the log tail broke on PR #3064 (2026-10-27):
 *   - blob storage ignored the Range header and sent each whole log, so eight
 *     failures came back as 1.9 MB and overran the server's reply relay;
 *   - the post-job steps after the failed one filled the tail, so the error the
 *     caller needed was cut off and only service-container teardown showed.
 *
 * The forge pr_info stage is canned through module_bus_stub; the REST reads and
 * the log fetch are served by the agent_http stubs below, keyed by URL.
 */
#include "modules/git/git_pr_api.h"
#include "support/module_bus_stub.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int git_cred_inject_resolve_token(const char *principal, const char *remote_url,
                                  const char *repo_dir, const char *preferred_token, char *out,
                                  size_t cap)
{
   (void)principal;
   (void)remote_url;
   (void)repo_dir;
   (void)preferred_token;
   if (!out || cap == 0)
      return 0;
   snprintf(out, cap, "test-token");
   return 1;
}

int strip_ai_attribution(char *text)
{
   (void)text;
   return 0;
}

#define TAIL_BYTES  6000
#define LOG_URL     "https://blob.example/job-log"
#define FAILURE_MSG "FAILED: build/obj/tests/unit-test-kb-workload-helper-posix"

static char *g_log; /* the whole job log, served regardless of any Range asked for */
static char g_range_seen[64];

static char *dup_str(const char *s)
{
   size_t n = strlen(s) + 1;
   char *d = malloc(n);
   assert(d);
   memcpy(d, s, n);
   return d;
}

int agent_http_get(const char *url, const char *headers, char **resp, int timeout_ms)
{
   (void)timeout_ms;
   *resp = NULL;
   if (strcmp(url, LOG_URL) == 0)
   {
      /* The signed URL must not carry the forge token. */
      assert(!headers || !strstr(headers, "Authorization"));
      snprintf(g_range_seen, sizeof(g_range_seen), "%s", headers ? headers : "");
      *resp = dup_str(g_log);
      return 200; /* Range ignored: the whole log, as on 2026-10-27 */
   }
   if (strstr(url, "/check-runs"))
   {
      *resp =
          dup_str("{\"check_runs\":[{\"name\":\"unit-tests (2/4)\",\"conclusion\":\"failure\","
                  "\"details_url\":\"https://github.com/acme/widgets/actions/runs/1/job/42\"}]}");
      return 200;
   }
   if (strstr(url, "/actions/jobs/42"))
   {
      *resp = dup_str(
          "{\"steps\":[{\"name\":\"Unit tests\",\"number\":9,\"conclusion\":\"failure\"}]}");
      return 200;
   }
   return 404;
}

int agent_http_get_location(const char *url, const char *extra_headers, char *location,
                            size_t location_cap, char **response_buf, int timeout_ms)
{
   (void)extra_headers;
   (void)timeout_ms;
   *response_buf = NULL;
   assert(strstr(url, "/actions/jobs/42/logs"));
   snprintf(location, location_cap, "%s", LOG_URL);
   return 302;
}

int agent_http_put(const char *url, const char *auth, const char *body, char **resp, int timeout_ms,
                   const char *accept)
{
   (void)url;
   (void)auth;
   (void)body;
   (void)resp;
   (void)timeout_ms;
   (void)accept;
   abort();
}

int agent_http_post_content_type(const char *url, const char *auth, const char *content_type,
                                 const char *body, char **resp, int timeout_ms, const char *accept)
{
   (void)url;
   (void)auth;
   (void)content_type;
   (void)body;
   (void)resp;
   (void)timeout_ms;
   (void)accept;
   abort();
}

/* A log shaped like the real one: a long failed step ending in the runner's error
 * marker, then post-job steps longer than the whole tail budget. */
static char *build_log(void)
{
   size_t cap = 2u * 1024u * 1024u;
   char *log = malloc(cap);
   assert(log);
   size_t len = 0;
   while (len < 1800000u)
      len += (size_t)snprintf(log + len, cap - len, "test_case_%zu... OK\n", len);
   len += (size_t)snprintf(log + len, cap - len,
                           "Unit test failures:\n" FAILURE_MSG "\n"
                           "make: *** [Makefile:2857: sanitizers] Error 2\n"
                           "##[error]Process completed with exit code 2.\n");
   for (int i = 0; i < 200; i++)
      len += (size_t)snprintf(log + len, cap - len,
                              "Post job cleanup. [command]/usr/bin/git config --local "
                              "--unset-all http.https://github.com/.extraheader %d\n",
                              i);
   return log;
}

int main(void)
{
   /* The forge stage seals its credential to egress's key (as in test_git_pr_stage.c). */
   module_bus_stub_reply_for_event(
       12295u, "{\"version\":1,\"key_id\":\"0123456789abcdef0123456789abcdef\","
               "\"public_key\":\"hSDwCYkwp1R0i33ctD73Wg2/Og0mOBr066SpjqqbTmo=\"}");
   module_bus_stub_reply("{\"pull\":{\"state\":\"open\",\"merged\":false,\"head_sha\":\"abc123\","
                         "\"head\":\"feature\",\"base\":\"testing\"}}");
   g_log = build_log();

   git_pr_failure_t rows[4];
   int n = 0;
   char err[512] = "";
   int rc = git_pr_failures_via_api_slug(NULL, "acme/widgets", 7, 4, 1, TAIL_BYTES, rows, &n, err,
                                         sizeof(err));
   if (rc != 0)
      fprintf(stderr, "pr failures: %s\n", err);
   assert(rc == 0);
   assert(n == 1);
   assert(strcmp(rows[0].failed_step, "Unit tests") == 0);
   assert(strstr(g_range_seen, "Range: bytes=-"));
   assert(rows[0].log_tail);

   /* Bounded however much the storage sent. */
   assert(strlen(rows[0].log_tail) <= TAIL_BYTES);
   /* Ends at the failed step, so the failure itself is in the tail ... */
   assert(strstr(rows[0].log_tail, FAILURE_MSG));
   assert(strstr(rows[0].log_tail, "##[error]Process completed with exit code 2."));
   /* ... and the post-job noise after it is not. */
   assert(!strstr(rows[0].log_tail, "Post job cleanup."));

   git_pr_failures_free(rows, n);
   free(g_log);
   puts("git pr failures log tail: bounded and cut at the failed step");
   return 0;
}
