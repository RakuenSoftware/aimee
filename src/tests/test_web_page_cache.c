/* test_web_page_cache.c -- the page cache and its key.
 *
 * The design decision under test is that the key is the URL ALONE, not
 * (url, query, budget). That is only sound because extraction is a deterministic
 * pure function re-run on every hit, so the cache supplies the document and
 * never freezes a policy decision. The consequence worth pinning: a page fetched
 * for one query is served for a completely different one. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db1.h"
#include "web_page_cache.h"

static void test_roundtrip(void)
{
   const char *url = "https://example.com/doc";
   assert(db1_web_page_put(url, "hello page text", "93.184.216.34") == 0);

   long age = -1;
   char pinned[DB1_WEB_PAGE_ADDR_LEN] = "";
   char *got = db1_web_page_get(url, &age, pinned, sizeof(pinned));
   assert(got && strcmp(got, "hello page text") == 0);
   assert(age >= 0);
   assert(strcmp(pinned, "93.184.216.34") == 0);
   free(got);
   printf("  PASS: store and retrieve, with age and pinned address\n");
}

/* The point of keying on the URL alone: the query is not part of identity. */
static void test_key_is_url_only(void)
{
   const char *url = "https://example.com/keytest";
   assert(db1_web_page_put(url, "body for any query", "1.1.1.1") == 0);
   /* nothing about a query was ever supplied; a later, different question
    * still hits, which is the entire hit-rate argument */
   char *got = db1_web_page_get(url, NULL, NULL, 0);
   assert(got && strcmp(got, "body for any query") == 0);
   free(got);
   printf("  PASS: key is the URL alone; any query hits a fetched page\n");
}

/* Canonicalisation decides what counts as the same page. */
static void test_canonicalisation(void)
{
   char a[512], b[512];

   /* scheme and host case are not identity */
   assert(db1_web_page_canonical_url("HTTPS://Example.COM/Path", a, sizeof(a)) == 0);
   assert(db1_web_page_canonical_url("https://example.com/Path", b, sizeof(b)) == 0);
   assert(strcmp(a, b) == 0);
   /* path case IS identity -- servers may distinguish it */
   assert(db1_web_page_canonical_url("https://example.com/path", b, sizeof(b)) == 0);
   assert(strcmp(a, b) != 0);

   /* a default port is not identity */
   assert(db1_web_page_canonical_url("https://example.com:443/x", a, sizeof(a)) == 0);
   assert(db1_web_page_canonical_url("https://example.com/x", b, sizeof(b)) == 0);
   assert(strcmp(a, b) == 0);
   /* a non-default port is */
   assert(db1_web_page_canonical_url("https://example.com:8443/x", b, sizeof(b)) == 0);
   assert(strcmp(a, b) != 0);

   /* the fragment never goes on the wire, so it cannot be identity */
   assert(db1_web_page_canonical_url("https://example.com/x#frag", a, sizeof(a)) == 0);
   assert(db1_web_page_canonical_url("https://example.com/x", b, sizeof(b)) == 0);
   assert(strcmp(a, b) == 0);

   /* the query string IS part of the resource */
   assert(db1_web_page_canonical_url("https://example.com/x?a=1", a, sizeof(a)) == 0);
   assert(db1_web_page_canonical_url("https://example.com/x?a=2", b, sizeof(b)) == 0);
   assert(strcmp(a, b) != 0);

   /* an empty path normalises so "host" and "host/" are one entry */
   assert(db1_web_page_canonical_url("https://example.com", a, sizeof(a)) == 0);
   assert(db1_web_page_canonical_url("https://example.com/", b, sizeof(b)) == 0);
   assert(strcmp(a, b) == 0);

   /* non-http schemes are not cacheable */
   assert(db1_web_page_canonical_url("file:///etc/passwd", a, sizeof(a)) != 0);
   assert(db1_web_page_canonical_url("gopher://x/", a, sizeof(a)) != 0);
   assert(db1_web_page_canonical_url("not a url", a, sizeof(a)) != 0);
   printf("  PASS: canonical key rules\n");
}

/* Two URLs that canonicalise the same must share one entry, not two. */
static void test_canonical_forms_share_an_entry(void)
{
   assert(db1_web_page_put("https://Shared.example.com:443/p", "shared body", "8.8.8.8") == 0);
   char *got = db1_web_page_get("https://shared.example.com/p#anchor", NULL, NULL, 0);
   assert(got && strcmp(got, "shared body") == 0);
   free(got);
   printf("  PASS: equivalent URL forms share one entry\n");
}

static void test_drop(void)
{
   const char *url = "https://example.com/dropme";
   assert(db1_web_page_put(url, "temporary", "1.1.1.1") == 0);
   assert(db1_web_page_get(url, NULL, NULL, 0) != NULL);
   db1_web_page_drop(url);
   assert(db1_web_page_get(url, NULL, NULL, 0) == NULL);
   printf("  PASS: drop removes an entry\n");
}

static void test_overwrite(void)
{
   const char *url = "https://example.com/overwrite";
   assert(db1_web_page_put(url, "first", "1.1.1.1") == 0);
   assert(db1_web_page_put(url, "second", "2.2.2.2") == 0);
   char pinned[DB1_WEB_PAGE_ADDR_LEN] = "";
   char *got = db1_web_page_get(url, NULL, pinned, sizeof(pinned));
   assert(got && strcmp(got, "second") == 0);
   assert(strcmp(pinned, "2.2.2.2") == 0);
   free(got);
   printf("  PASS: refetch overwrites body and pinned address\n");
}

/* A miss must be a miss, not an error the caller has to handle. */
static void test_miss_is_quiet(void)
{
   assert(db1_web_page_get("https://example.com/never-fetched", NULL, NULL, 0) == NULL);
   assert(db1_web_page_get(NULL, NULL, NULL, 0) == NULL);
   assert(db1_web_page_get("garbage", NULL, NULL, 0) == NULL);
   /* an unstorable URL must not blow up a write either */
   assert(db1_web_page_put("file:///etc/passwd", "x", "1.1.1.1") != 0);
   db1_web_page_drop("also not a url");
   printf("  PASS: misses and unusable URLs are quiet, never fatal\n");
}

int main(void)
{
   assert(db1_init(":memory:") == 0);
   test_roundtrip();
   test_key_is_url_only();
   test_canonicalisation();
   test_canonical_forms_share_an_entry();
   test_drop();
   test_overwrite();
   test_miss_is_quiet();
   printf("web_page_cache: all tests passed\n");
   return 0;
}
