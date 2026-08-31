/* test_git_pr_stage.c — the C side of the git-forge-request seam.
 *
 * The forge call itself is the module's job and is tested there
 * (server-go/modules/git/forge_request_test.go). What is NOT covered there, and
 * is what this file pins, is the TRANSLATION back into the return codes callers
 * switch on: 0 merged, 1 already merged, 2 retryable, 3 terminal conflict, -1
 * error. Getting that mapping wrong is invisible to both sides — the module
 * answers correctly and the caller still does the wrong thing.
 *
 * The stage reply is canned through module_bus_stub, so these assertions are
 * about the mapping and nothing else. The HTTP entry points are stubbed to
 * ABORT: a migrated op that quietly fell back to talking to GitHub itself would
 * otherwise pass this suite while defeating the point of the migration.
 */
#include "modules/git/git_pr_api.h"
#include "support/module_bus_stub.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs -------------------------------------------------------------- */

/* A credential is always available: this suite is not about resolution. */
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

/* The real one lives in util.c, which would drag run_cmd/safe_exec into this
 * binary for a policy that is tested there. Counting the calls instead keeps the
 * link small AND pins the thing that actually matters here: the standing
 * no-AI-attribution strip must still run on the migrated path, before the body
 * is handed to the stage. Moving code is exactly when such a step gets dropped.
 */
static int g_strip_calls;

int strip_ai_attribution(char *text)
{
   (void)text;
   g_strip_calls++;
   return 0;
}

/* Any direct HTTP from a migrated op is a bug, so make it loud rather than let
 * the suite pass on a silent fallback. */
static void http_forbidden(const char *which)
{
   fprintf(stderr, "migrated op reached HTTP directly via %s\n", which);
   abort();
}

int agent_http_get(const char *url, const char *headers, char **resp, int timeout_ms)
{
   (void)url;
   (void)headers;
   (void)resp;
   (void)timeout_ms;
   http_forbidden("agent_http_get");
   return -1;
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
   http_forbidden("agent_http_put");
   return -1;
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
   http_forbidden("agent_http_post_content_type");
   return -1;
}

/* --- helpers ------------------------------------------------------------ */

#define SLUG "acme/widgets"

static int merge_with(const char *reply_json, char *sha, size_t sha_cap, char *err, size_t errlen)
{
   module_bus_stub_reply(reply_json);
   return git_pr_merge_via_api_slug_ex(NULL, SLUG, 7, "merge", NULL, sha, sha_cap, err, errlen);
}

int main(void)
{
   char err[512];
   char sha[128];

   /* The core asks the real egress process for this key before each request.
    * Pin a valid X25519 public key here; the secret itself must only appear as
    * authenticated ciphertext in the later forge-stage body. */
   module_bus_stub_reply_for_event(
       12295u, "{\"version\":1,\"key_id\":\"0123456789abcdef0123456789abcdef\","
               "\"public_key\":\"hSDwCYkwp1R0i33ctD73Wg2/Og0mOBr066SpjqqbTmo=\"}");

   /* A merge reports the commit from the stage reply, so no second lookup. */
   assert(merge_with("{\"status\":200,\"merged\":true,\"merge_sha\":\"deadbeef\"}", sha,
                     sizeof(sha), err, sizeof(err)) == 0);
   assert(strcmp(sha, "deadbeef") == 0);
   assert(strstr(module_bus_stub_last_request(), "\"credential\"") != NULL);
   assert(strstr(module_bus_stub_last_request(), "\"token\"") == NULL);
   assert(strstr(module_bus_stub_last_request(), "test-token") == NULL);

   /* Already merged is NOT an error: a caller that retried would open a second
    * merge of work already on the base. */
   assert(merge_with("{\"status\":405,\"already_merged\":true}", sha, sizeof(sha), err,
                     sizeof(err)) == 1);

   /* Retryable — a lost race, which a retry wins. */
   assert(merge_with("{\"status\":405,\"error\":\"pr merge: Base branch was modified (HTTP 405)\","
                     "\"retryable\":true}",
                     sha, sizeof(sha), err, sizeof(err)) == 2);
   assert(strstr(err, "Base branch was modified") != NULL);

   /* Terminal conflict — retrying reproduces it exactly, so it must not be
    * classified as retryable. This is the pair the whole split exists for. */
   assert(merge_with("{\"status\":409,\"error\":\"pr merge: Pull Request has merge conflicts "
                     "(HTTP 409)\",\"conflict\":true}",
                     sha, sizeof(sha), err, sizeof(err)) == 3);
   assert(strstr(err, "merge conflicts") != NULL);

   /* An unclassified refusal is an error, and carries the forge's own words. */
   assert(merge_with("{\"status\":403,\"error\":\"pr merge: Resource not accessible (HTTP 403)\"}",
                     sha, sizeof(sha), err, sizeof(err)) == -1);
   assert(strstr(err, "403") != NULL);

   /* A merge reply with no sha still succeeds; sha is simply left empty. */
   assert(merge_with("{\"status\":200,\"merged\":true}", sha, sizeof(sha), err, sizeof(err)) == 0);
   assert(sha[0] == '\0');

   /* THE DISTINCTION THAT MATTERS MOST: an unreachable module is not a refused
    * merge. Reported as "the forge said no", a caller stops retrying a merge
    * that was never attempted. */
   module_bus_stub_absent();
   assert(git_pr_merge_via_api_slug_ex(NULL, SLUG, 7, "merge", NULL, sha, sizeof(sha), err,
                                       sizeof(err)) == -1);
   assert(strstr(err, "could not be reached") != NULL);

   /* An unrecognised method is refused HERE, before anything is sent: coercing
    * it to a squash would rewrite history the caller did not ask to rewrite. */
   module_bus_stub_reply("{\"status\":200,\"merged\":true}");
   int before = module_bus_stub_calls();
   assert(git_pr_merge_via_api_slug_ex(NULL, SLUG, 7, "octopus", NULL, sha, sizeof(sha), err,
                                       sizeof(err)) == -1);
   assert(module_bus_stub_calls() == before); /* nothing was sent */
   assert(strstr(err, "octopus") != NULL);

   /* A non-positive number is likewise refused before any call. */
   before = module_bus_stub_calls();
   assert(git_pr_merge_via_api_slug_ex(NULL, SLUG, 0, "merge", NULL, sha, sizeof(sha), err,
                                       sizeof(err)) == -1);
   assert(module_bus_stub_calls() == before);

   /* --- pr_create ------------------------------------------------------- */

   char urlbuf[512];

   /* The created PR's URL comes from the stage reply's pull summary. */
   module_bus_stub_reply("{\"status\":201,\"pull\":{\"number\":42,\"url\":"
                         "\"https://github.com/acme/widgets/pull/42\"}}");
   g_strip_calls = 0;
   assert(git_pr_create_via_api_slug(NULL, SLUG, "feat", "testing", "A title", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == 0);
   assert(strcmp(urlbuf, "https://github.com/acme/widgets/pull/42") == 0);
   /* The body was put through the attribution strip on the way to the stage. */
   assert(g_strip_calls == 1);

   /* A refusal must arrive as the forge's OWN words: "A pull request already
    * exists" sends an operator somewhere useful, "HTTP 422" does not. */
   module_bus_stub_reply(
       "{\"status\":422,\"error\":\"pr create: A pull request already exists (HTTP 422)\"}");
   assert(git_pr_create_via_api_slug(NULL, SLUG, "feat", "testing", "A title", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == -1);
   assert(strstr(err, "already exists") != NULL);
   assert(urlbuf[0] == '\0'); /* a refused create reports no URL */

   /* A 2xx carrying no URL is not a success: reporting one would hand the caller
    * an empty link for a PR it cannot confirm was opened. */
   module_bus_stub_reply("{\"status\":201,\"pull\":{\"number\":42}}");
   assert(git_pr_create_via_api_slug(NULL, SLUG, "feat", "testing", "A title", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == -1);
   assert(err[0] != '\0');

   /* Unreachable module is not a refused create. */
   module_bus_stub_absent();
   assert(git_pr_create_via_api_slug(NULL, SLUG, "feat", "testing", "A title", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == -1);
   assert(strstr(err, "could not be reached") != NULL);

   /* Without a checkout there is nothing to infer head/base/title from, and
    * guessing is how a PR lands on the wrong base. Refuse before sending. */
   module_bus_stub_reply("{\"status\":201,\"pull\":{\"url\":\"u\"}}");
   before = module_bus_stub_calls();
   assert(git_pr_create_via_api_slug(NULL, SLUG, "", "testing", "A title", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == -1);
   assert(git_pr_create_via_api_slug(NULL, SLUG, "feat", "", "A title", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == -1);
   assert(git_pr_create_via_api_slug(NULL, SLUG, "feat", "testing", "", "body", 0, urlbuf,
                                     sizeof(urlbuf), err, sizeof(err)) == -1);
   assert(module_bus_stub_calls() == before);

   /* --- pr_info --------------------------------------------------------- */

   git_pr_info_t info;

   module_bus_stub_reply(
       "{\"status\":200,\"pull\":{\"number\":7,\"state\":\"closed\",\"title\":\"T\","
       "\"merged\":true,\"merged_at\":\"2026-08-10T13:17:33Z\",\"merge_state\":\"CLEAN\","
       "\"url\":\"https://github.com/acme/widgets/pull/7\",\"head\":\"feat\","
       "\"head_sha\":\"deadbeef\",\"base\":\"testing\",\"mergeable\":true}}");
   assert(git_pr_info_via_api_slug(NULL, SLUG, 7, &info, err, sizeof(err)) == 0);
   assert(info.open == 0 && info.merged == 1);
   assert(strcmp(info.head_sha, "deadbeef") == 0);
   assert(strcmp(info.head, "feat") == 0 && strcmp(info.base, "testing") == 0);
   assert(strcmp(info.merge_state, "CLEAN") == 0);
   assert(strcmp(info.merged_at, "2026-08-10T13:17:33Z") == 0);
   assert(strcmp(info.html_url, "https://github.com/acme/widgets/pull/7") == 0);
   assert(info.mergeable == 1);

   /* MERGEABLE IS THREE-VALUED. An ABSENT mergeable means the forge is still
    * computing the merge; it must stay -1 rather than collapse to 0, or a caller
    * reads "still computing" as "conflicted" and abandons a mergeable PR. */
   module_bus_stub_reply("{\"status\":200,\"pull\":{\"number\":7,\"state\":\"open\","
                         "\"head\":\"feat\",\"head_sha\":\"abc\",\"base\":\"testing\"}}");
   assert(git_pr_info_via_api_slug(NULL, SLUG, 7, &info, err, sizeof(err)) == 0);
   assert(info.mergeable == -1);
   assert(info.open == 1 && info.merged == 0);
   assert(info.merged_at[0] == '\0'); /* never merged -> no timestamp */

   /* An explicit false is a real answer and must NOT read as "unknown". */
   module_bus_stub_reply("{\"status\":200,\"pull\":{\"number\":7,\"state\":\"open\","
                         "\"head\":\"feat\",\"head_sha\":\"abc\",\"base\":\"testing\","
                         "\"mergeable\":false}}");
   assert(git_pr_info_via_api_slug(NULL, SLUG, 7, &info, err, sizeof(err)) == 0);
   assert(info.mergeable == 0);

   /* Missing refs fail the whole call: head_sha is what a merge's drift check is
    * made against, so proceeding without it would silently disable that. */
   module_bus_stub_reply(
       "{\"status\":200,\"pull\":{\"number\":7,\"state\":\"open\",\"head\":\"feat\"}}");
   assert(git_pr_info_via_api_slug(NULL, SLUG, 7, &info, err, sizeof(err)) == -1);
   assert(strstr(err, "missing required refs") != NULL);

   /* A refusal carries the forge's own message. */
   module_bus_stub_reply("{\"status\":404,\"error\":\"pr info: Not Found (HTTP 404)\"}");
   assert(git_pr_info_via_api_slug(NULL, SLUG, 7, &info, err, sizeof(err)) == -1);
   assert(strstr(err, "Not Found") != NULL);

   module_bus_stub_absent();
   assert(git_pr_info_via_api_slug(NULL, SLUG, 7, &info, err, sizeof(err)) == -1);
   assert(strstr(err, "could not be reached") != NULL);

   /* --- pr_find_open ---------------------------------------------------- */

   int number = 0;

   module_bus_stub_reply("{\"status\":200,\"pull\":{\"number\":5,\"url\":"
                         "\"https://github.com/acme/widgets/pull/5\"},\"pulls\":[{\"number\":5}]}");
   assert(git_pr_find_open_via_api_slug(NULL, SLUG, "feat", "testing", urlbuf, sizeof(urlbuf),
                                        &number, err, sizeof(err)) == 1);
   assert(number == 5);
   assert(strcmp(urlbuf, "https://github.com/acme/widgets/pull/5") == 0);

   /* NO MATCH IS NOT AN ERROR. 0 is the answer the caller acts on by opening a
    * PR; returning -1 would make it give up instead. The stage omits `pull`
    * entirely when nothing matched. */
   module_bus_stub_reply("{\"status\":200}");
   number = 7;
   assert(git_pr_find_open_via_api_slug(NULL, SLUG, "feat", "testing", urlbuf, sizeof(urlbuf),
                                        &number, err, sizeof(err)) == 0);
   assert(number == 0 && urlbuf[0] == '\0');
   assert(err[0] == '\0'); /* "not found" carries no error text */

   /* A real refusal still is one. */
   module_bus_stub_reply("{\"status\":404,\"error\":\"pr list: Not Found (HTTP 404)\"}");
   assert(git_pr_find_open_via_api_slug(NULL, SLUG, "feat", "testing", urlbuf, sizeof(urlbuf),
                                        &number, err, sizeof(err)) == -1);
   assert(strstr(err, "Not Found") != NULL);

   /* The head/base guards refuse before anything is sent. */
   module_bus_stub_reply("{\"status\":200}");
   before = module_bus_stub_calls();
   assert(git_pr_find_open_via_api_slug(NULL, SLUG, "", "testing", urlbuf, sizeof(urlbuf), &number,
                                        err, sizeof(err)) == -1);
   assert(git_pr_find_open_via_api_slug(NULL, SLUG, "feat&x", "testing", urlbuf, sizeof(urlbuf),
                                        &number, err, sizeof(err)) == -1);
   assert(module_bus_stub_calls() == before);

   /* --- pr_list_open ---------------------------------------------------- */

   git_pr_list_item_t rows[4];
   int count = -1;

   module_bus_stub_reply("{\"status\":200,\"pulls\":["
                         "{\"number\":9,\"state\":\"open\",\"title\":\"A\",\"head\":\"one\"},"
                         "{\"number\":8,\"state\":\"closed\",\"title\":\"B\",\"head\":\"two\","
                         "\"merged_at\":\"2026-08-10T00:00:00Z\"}]}");
   assert(git_pr_list_open_via_api_slug(NULL, SLUG, 4, rows, &count, err, sizeof(err)) == 0);
   assert(count == 2);
   assert(rows[0].number == 9 && strcmp(rows[0].state, "OPEN") == 0);
   assert(strcmp(rows[0].head, "one") == 0 && strcmp(rows[0].title, "A") == 0);
   /* A merged PR carries the closed state; callers render MERGED. */
   assert(rows[1].number == 8 && strcmp(rows[1].state, "MERGED") == 0);

   /* AN EMPTY LIST IS A RESULT, NOT A FAILURE. The stage omits an empty `pulls`
    * entirely, and reporting an error would turn "no open PRs" into "the
    * listing is broken". */
   module_bus_stub_reply("{\"status\":200}");
   count = -1;
   assert(git_pr_list_open_via_api_slug(NULL, SLUG, 4, rows, &count, err, sizeof(err)) == 0);
   assert(count == 0);

   /* The limit is honoured even if the stage returns more rows than asked. */
   module_bus_stub_reply("{\"status\":200,\"pulls\":[{\"number\":1},{\"number\":2},"
                         "{\"number\":3},{\"number\":4},{\"number\":5}]}");
   assert(git_pr_list_open_via_api_slug(NULL, SLUG, 3, rows, &count, err, sizeof(err)) == 0);
   assert(count == 3);

   module_bus_stub_reply("{\"status\":403,\"error\":\"pr list: Forbidden (HTTP 403)\"}");
   assert(git_pr_list_open_via_api_slug(NULL, SLUG, 4, rows, &count, err, sizeof(err)) == -1);
   assert(strstr(err, "Forbidden") != NULL);

   module_bus_stub_absent();
   assert(git_pr_list_open_via_api_slug(NULL, SLUG, 4, rows, &count, err, sizeof(err)) == -1);
   assert(strstr(err, "could not be reached") != NULL);

   printf("git_pr_stage: all tests passed\n");
   return 0;
}
