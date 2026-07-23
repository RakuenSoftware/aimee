/* test_web_search_fusion.c -- search fusion, and the contract it must not break.
 *
 * Fusion fetches the top search results through the guarded egress path and
 * appends query-relevant spans. The load-bearing property is that it is PURELY
 * ADDITIVE: anything parsing today's "[N] title -- url / snippet" block keeps
 * working, and removing the feature is deleting one section. That is asserted
 * here as a byte-identical prefix rather than by inspection.
 *
 * The transport is stubbed, so no network is touched and the test is
 * deterministic. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web_search.h"
#include "web_egress.h"
#include "web_extract.h"

static const char *FAKE_PAGE =
    "<html><body><h1>Docs</h1>"
    "<p>Intro paragraph with padding so the chunkless extractor has room to work here and "
    "there.</p>"
    "<p>The configuration accepts retry_budget_ms which bounds how long the client retries.</p>"
    "<p>Closing notes about licensing, padded out to form a distinct region of the document.</p>"
    "</body></html>";

/* stub the guarded transport: search page returns DDG-shaped HTML, result pages
 * return the fake doc */
char *web_egress_fetch(const char *url, web_egress_policy_t policy, const char *extra_headers,
                       int timeout_ms, size_t max_bytes, const char **err)
{
   (void)policy;
   (void)extra_headers;
   (void)timeout_ms;
   (void)max_bytes;
   if (err)
      *err = NULL;
   if (strstr(url, "duckduckgo.com"))
      return strdup("<div class=\"result__body\"><a class=\"result__a\" "
                    "href=\"https://example.com/doc1\">Doc One</a>"
                    "<a class=\"result__snippet\">snippet one text</a></div>"
                    "<div class=\"result__body\"><a class=\"result__a\" "
                    "href=\"https://example.com/doc2\">Doc Two</a>"
                    "<a class=\"result__snippet\">snippet two text</a></div>");
   return strdup(FAKE_PAGE);
}
int web_egress_addr_blocked(const struct sockaddr *sa)
{
   (void)sa;
   return 0;
}
int web_egress_private_endpoint_allowed(void)
{
   return 0;
}

/* minimal stubs: the test drives the duckduckgo path only */
#include "config.h"
int config_load(config_t *c)
{
   memset(c, 0, sizeof(*c));
   return 0;
}
int http_retry_post(const char *u, const char *a, const char *b, char **r, int t)
{
   (void)u;
   (void)a;
   (void)b;
   (void)r;
   (void)t;
   return -1;
}

int main(void)
{
   char *plain = web_search_ex("retry_budget_ms", 5, 0, NULL);
   char *fused = web_search_ex("retry_budget_ms", 5, 1, NULL);
   assert(plain && fused);

   /* the snippet block must be a byte-identical PREFIX of the fused output */
   size_t lp = strlen(plain);
   if (strncmp(plain, fused, lp) != 0)
   {
      fprintf(stderr, "--- plain ---\n%s\n--- fused ---\n%s\n", plain, fused);
      assert(0 && "fusion changed the snippet block");
   }
   printf("  PASS: snippet block byte-identical with fusion on and off\n");

   assert(strlen(fused) > lp);
   assert(strstr(fused, "extracted page spans"));
   assert(strstr(fused, "untrusted retrieved content"));
   printf("  PASS: spans appended and fenced as untrusted\n");

   assert(strstr(fused, "retry_budget_ms"));
   printf("  PASS: fused output contains the query term from the fetched page\n");

   /* extract_query override */
   char *ov = web_search_ex("retry_budget_ms", 5, 1, "licensing");
   assert(ov && strstr(ov, "licensing"));
   printf("  PASS: extract_query override drives extraction\n");

   free(plain);
   free(fused);
   free(ov);
   printf("web_search_fusion: all tests passed\n");
   return 0;
}
