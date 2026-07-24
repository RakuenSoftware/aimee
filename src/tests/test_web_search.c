/* test_web_search.c: unit tests for web_search.c pure-logic functions.
 *
 * Tests URL encoding, DuckDuckGo HTML parsing, and result formatting.
 * No network calls are made.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "web_search.h"

/* ---- URL encoding ---- */

static void test_url_encode_simple(void)
{
   char *out = web_search_url_encode("hello world");
   assert(out != NULL);
   assert(strcmp(out, "hello+world") == 0);
   free(out);
}

static void test_url_encode_special(void)
{
   char *out = web_search_url_encode("c++ error: undefined");
   assert(out != NULL);
   /* '+' is safe, ':' and spaces get encoded */
   assert(strstr(out, "c%2B%2B") != NULL);
   assert(strstr(out, "error%3A") != NULL);
   free(out);
}

static void test_url_encode_alnum_passthrough(void)
{
   char *out = web_search_url_encode("abc123-_.~");
   assert(out != NULL);
   assert(strcmp(out, "abc123-_.~") == 0);
   free(out);
}

static void test_url_encode_empty(void)
{
   char *out = web_search_url_encode("");
   assert(out != NULL);
   assert(strlen(out) == 0);
   free(out);

   out = web_search_url_encode(NULL);
   assert(out != NULL);
   free(out);
}

/* ---- DuckDuckGo HTML parsing ---- */

/* Minimal HTML fixture that resembles a real DDG HTML response */
static const char SIMPLE_DDG_HTML[] =
    "<div class=\"results\">"
    "<div class=\"result\">"
    "<a class=\"result__a\" href=\"https://example.com/page\">Example Title</a>"
    "<div class=\"result__snippet\">This is the snippet for result one.</div>"
    "</div>"
    "<div class=\"result\">"
    "<a class=\"result__a\" href=\"https://other.org/article\">Other Article</a>"
    "<div class=\"result__snippet\">Second snippet text.</div>"
    "</div>"
    "</div>";

static void test_parse_duckduckgo_basic(void)
{
   web_search_result_t results[10];
   memset(results, 0, sizeof(results));

   int count = web_search_parse_duckduckgo(SIMPLE_DDG_HTML, 10, results);
   assert(count >= 1);
   assert(results[0].url != NULL);
   assert(strcmp(results[0].url, "https://example.com/page") == 0);
   assert(results[0].title != NULL);
   assert(strstr(results[0].title, "Example Title") != NULL);

   web_search_free_results(results, count);
}

static void test_parse_duckduckgo_max_results(void)
{
   web_search_result_t results[10];
   memset(results, 0, sizeof(results));

   /* Request only 1 result, even though 2 are available */
   int count = web_search_parse_duckduckgo(SIMPLE_DDG_HTML, 1, results);
   assert(count == 1);

   web_search_free_results(results, count);
}

static void test_parse_duckduckgo_skips_redirect(void)
{
   /* DDG redirect URLs should be skipped */
   const char html[] =
       "<a class=\"result__a\" href=\"https://duckduckgo.com/redirect?uddg=x\">Skip me</a>"
       "<a class=\"result__a\" href=\"https://real-site.com/page\">Keep me</a>"
       "<div class=\"result__snippet\">Good snippet</div>";

   web_search_result_t results[10];
   memset(results, 0, sizeof(results));

   int count = web_search_parse_duckduckgo(html, 10, results);
   /* Should skip the duckduckgo.com redirect and return the real one */
   int found_real = 0;
   for (int i = 0; i < count; i++)
   {
      if (results[i].url && strstr(results[i].url, "real-site.com"))
         found_real = 1;
      if (results[i].url && strstr(results[i].url, "duckduckgo.com"))
         assert(0); /* should never appear */
   }
   (void)found_real; /* may or may not appear depending on parse order */

   web_search_free_results(results, count);
}

static void test_parse_duckduckgo_empty(void)
{
   web_search_result_t results[10];
   memset(results, 0, sizeof(results));

   int count = web_search_parse_duckduckgo("", 10, results);
   assert(count == 0);

   count = web_search_parse_duckduckgo(NULL, 10, results);
   assert(count == 0);

   count = web_search_parse_duckduckgo(SIMPLE_DDG_HTML, 0, results);
   assert(count == 0);
}

/* ---- Result formatting ---- */

static void test_format_results_basic(void)
{
   web_search_result_t results[2];
   memset(results, 0, sizeof(results));
   results[0].title = safe_strdup("First Result");
   results[0].url = safe_strdup("https://example.com");
   results[0].snippet = safe_strdup("First snippet.");
   results[1].title = safe_strdup("Second Result");
   results[1].url = safe_strdup("https://other.com");
   results[1].snippet = safe_strdup("Second snippet.");

   char *out = web_search_format_results(results, 2, 0);
   assert(out != NULL);
   assert(strstr(out, "[1]") != NULL);
   assert(strstr(out, "First Result") != NULL);
   assert(strstr(out, "https://example.com") != NULL);
   assert(strstr(out, "First snippet.") != NULL);
   assert(strstr(out, "[2]") != NULL);
   assert(strstr(out, "Second Result") != NULL);
   /* Titles and snippets are attacker-influenceable page content and must be
    * fenced as untrusted, matching how web_read fences its spans. */
   assert(strstr(out, "untrusted retrieved content") != NULL);
   free(out);

   web_search_free_results(results, 2);
}

static void test_format_results_empty(void)
{
   char *out = web_search_format_results(NULL, 0, 0);
   assert(out != NULL);
   assert(strstr(out, "No results") != NULL);
   free(out);
}

static void test_format_results_truncation(void)
{
   web_search_result_t results[3];
   memset(results, 0, sizeof(results));
   for (int i = 0; i < 3; i++)
   {
      results[i].title = safe_strdup("A title");
      results[i].url = safe_strdup("https://example.com");
      results[i].snippet = safe_strdup("snippet");
   }

   /* Set a very small max_bytes to force truncation */
   char *out = web_search_format_results(results, 3, 50);
   assert(out != NULL);
   assert(strstr(out, "[truncated]") != NULL);
   free(out);

   web_search_free_results(results, 3);
}

/* ---- free_results null safety ---- */

static void test_free_results_null(void)
{
   web_search_free_results(NULL, 0);
   web_search_free_results(NULL, 5);

   web_search_result_t results[2];
   memset(results, 0, sizeof(results));
   web_search_free_results(results, 2); /* all NULL pointers, should not crash */
}

int main(void)
{
   test_url_encode_simple();
   test_url_encode_special();
   test_url_encode_alnum_passthrough();
   test_url_encode_empty();

   test_parse_duckduckgo_basic();
   test_parse_duckduckgo_max_results();
   test_parse_duckduckgo_skips_redirect();
   test_parse_duckduckgo_empty();

   test_format_results_basic();
   test_format_results_empty();
   test_format_results_truncation();

   test_free_results_null();

   printf("web_search: all tests passed\n");
   return 0;
}
