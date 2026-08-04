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
#include <ctype.h>
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
char *web_egress_fetch_pinned(const char *url, web_egress_policy_t policy,
                              const char *extra_headers, int timeout_ms, size_t max_bytes,
                              char *pinned_out, size_t pinned_out_len, const char **err)
{
   if (pinned_out && pinned_out_len)
      snprintf(pinned_out, pinned_out_len, "93.184.216.34");
   return web_egress_fetch(url, policy, extra_headers, timeout_ms, max_bytes, err);
}

/* Cache stubbed to always miss. This suite is about the FUSION contract; a real
 * cache would make the second call in the process serve stored bytes and stop
 * exercising the fetch path. Cache behaviour has its own suite. */
char *db1_web_page_get(const char *url, long *age, char *pin, size_t pinlen)
{
   (void)url;
   if (age)
      *age = -1;
   if (pin && pinlen)
      pin[0] = 0;
   return NULL;
}
int db1_web_page_put(const char *url, const char *body, const char *pin)
{
   (void)url;
   (void)body;
   (void)pin;
   return 0;
}
void db1_web_page_drop(const char *url)
{
   (void)url;
}

/* Dedup keys pages by the cache's canonical form, so the fusion path reaches
 * this even with the cache itself stubbed out. Stubbed identically to the real
 * one for the shapes this suite uses: scheme and host lowercased, everything
 * after kept verbatim. Enough for "two different URLs stay two results", which
 * is all this suite asks of it -- the canonical rules have their own suite. */
int db1_web_page_canonical_url(const char *url, char *out, size_t out_len)
{
   if (!url || !out || out_len == 0)
      return -1;
   const char *sep = strstr(url, "://");
   if (!sep)
      return -1;
   size_t n = 0;
   for (const char *p = url; *p && n + 1 < out_len; p++)
   {
      /* lowercase through the authority, then copy verbatim */
      int in_authority = (p <= sep + 2) || !strchr("/?#", *p);
      const char *slash = strchr(sep + 3, '/');
      if (slash && p >= slash)
         in_authority = 0;
      out[n++] = in_authority ? (char)tolower((unsigned char)*p) : *p;
   }
   out[n] = '\0';
   return 0;
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

/* minimal stubs: the test drives the duckduckgo path only.
 *
 * These replace a config_load stub that zeroed the whole struct. Returning 0
 * from each accessor is the same contract — web_search falls back to its
 * HTTP_RETRY_* defaults — but stubs one value per knob instead of requiring the
 * test to know config_t's shape. */
#include "config.h"
int config_load(config_t *c)
{
   memset(c, 0, sizeof(*c));
   return 0;
}
/* run_engine reads these two directly now. Empty is what the zeroed struct gave
 * it, and both engines are skipped as unconfigured -- the duckduckgo path this
 * suite drives is unaffected. */
/* Engine selection: empty backends + empty backend, so web_search falls through
 * to its duckduckgo default -- the path this suite drives, and what the zeroed
 * config_t the old stub returned produced. */
const char *config_search_backends(void)
{
   return "";
}
const char *config_search_backend(void)
{
   return "";
}

/* run_engine takes the _copy form (the value crosses an HTTP round trip). Empty
 * is what the zeroed struct gave it: both engines skipped as unconfigured. */
size_t config_search_tavily_api_key_copy(char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   return n;
}
size_t config_search_searxng_url_copy(char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   return n;
}
int config_retry_max_attempts(void)
{
   return 0;
}
int config_retry_base_ms(void)
{
   return 0;
}
int config_retry_max_ms(void)
{
   return 0;
}
int http_retry_post(const char *u, const char *a, const char *b, char **r, int t,
                    const char *headers, int attempts, int base_ms, int max_ms)
{
   (void)u;
   (void)a;
   (void)b;
   (void)r;
   (void)t;
   (void)headers;
   (void)attempts;
   (void)base_ms;
   (void)max_ms;
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
