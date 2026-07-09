/* test_td_search_render.c — the pure render/extract helpers behind the kb_search
 * tool. These turn the /v1/search {"hits":[...]} envelope into (a) the text block
 * the agent tool returns and (b) the (doc_id, excerpt) pairs the learning-to-rank
 * outcome capture attributes. No DB, no network — just cJSON in, string/count out.
 *
 * Covers:
 *   1. render: multiple hits -> header + numbered lines with score + excerpt.
 *   2. render: empty / null / non-array hits -> a "no results" line (never error).
 *   3. render: missing score / excerpt fields -> line still emitted, no crash.
 *   4. extract: pulls doc_id + excerpt pairs, honours max, skips id<=0 / non-number.
 *   5. extract: null args / max<=0 -> 0.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/cJSON.h"
#include "../headers/td_search_render.h"

static cJSON *hit(const char *aid, double score, int64_t doc_id, const char *excerpt)
{
   cJSON *h = cJSON_CreateObject();
   if (aid)
      cJSON_AddStringToObject(h, "artifact_id", aid);
   if (score >= 0)
      cJSON_AddNumberToObject(h, "score", score);
   if (doc_id != 0)
      cJSON_AddNumberToObject(h, "doc_id", (double)doc_id);
   if (excerpt)
      cJSON_AddStringToObject(h, "excerpt", excerpt);
   return h;
}

static void test_render_multiple(void)
{
   cJSON *hits = cJSON_CreateArray();
   cJSON_AddItemToArray(hits, hit("docs/a.md", 0.912, 11, "alpha snippet"));
   cJSON_AddItemToArray(hits, hit("docs/b.md", 0.501, 22, "beta snippet"));

   char *s = td_render_search_hits(hits, "how do widgets work");
   assert(s);
   assert(strstr(s, "how do widgets work"));
   assert(strstr(s, "(2)"));
   assert(strstr(s, "1. docs/a.md"));
   assert(strstr(s, "(score 0.912)"));
   assert(strstr(s, "alpha snippet"));
   assert(strstr(s, "2. docs/b.md"));
   assert(strstr(s, "beta snippet"));
   free(s);
   cJSON_Delete(hits);
   printf("  ok: render multiple\n");
}

static void test_render_empty(void)
{
   cJSON *empty = cJSON_CreateArray();
   char *s = td_render_search_hits(empty, "nothing here");
   assert(s);
   assert(strstr(s, "No knowledge-base results"));
   assert(strstr(s, "nothing here"));
   free(s);
   cJSON_Delete(empty);

   /* null / non-array must not error — same "no results" contract. */
   char *sn = td_render_search_hits(NULL, "q");
   assert(sn && strstr(sn, "No knowledge-base results"));
   free(sn);

   cJSON *obj = cJSON_CreateObject();
   char *so = td_render_search_hits(obj, "q");
   assert(so && strstr(so, "No knowledge-base results"));
   free(so);
   cJSON_Delete(obj);
   printf("  ok: render empty/null/non-array\n");
}

static void test_render_missing_fields(void)
{
   cJSON *hits = cJSON_CreateArray();
   /* no score, no excerpt, no artifact_id */
   cJSON_AddItemToArray(hits, hit(NULL, -1, 5, NULL));
   char *s = td_render_search_hits(hits, "q");
   assert(s);
   assert(strstr(s, "1. (unknown)"));
   assert(!strstr(s, "score")); /* score line omitted when absent */
   free(s);
   cJSON_Delete(hits);
   printf("  ok: render missing fields\n");
}

static void test_extract(void)
{
   cJSON *hits = cJSON_CreateArray();
   cJSON_AddItemToArray(hits, hit("a", 0.9, 11, "ex-a"));
   cJSON_AddItemToArray(hits, hit("b", 0.8, 0, "no-doc")); /* doc_id 0 -> skipped */
   cJSON_AddItemToArray(hits, hit("c", 0.7, 33, NULL));    /* null excerpt -> "" */
   cJSON_AddItemToArray(hits, hit("d", 0.6, 44, "ex-d"));

   int64_t ids[8];
   const char *snips[8];
   int n = td_extract_hit_docs(hits, ids, snips, 8);
   assert(n == 3);
   assert(ids[0] == 11 && strcmp(snips[0], "ex-a") == 0);
   assert(ids[1] == 33 && strcmp(snips[1], "") == 0);
   assert(ids[2] == 44 && strcmp(snips[2], "ex-d") == 0);

   /* max caps the count */
   int n2 = td_extract_hit_docs(hits, ids, snips, 2);
   assert(n2 == 2);
   assert(ids[0] == 11 && ids[1] == 33);

   cJSON_Delete(hits);
   printf("  ok: extract doc pairs\n");
}

static void test_extract_guards(void)
{
   int64_t ids[4];
   const char *snips[4];
   cJSON *hits = cJSON_CreateArray();
   cJSON_AddItemToArray(hits, hit("a", 0.9, 11, "ex"));

   assert(td_extract_hit_docs(NULL, ids, snips, 4) == 0);
   assert(td_extract_hit_docs(hits, NULL, snips, 4) == 0);
   assert(td_extract_hit_docs(hits, ids, NULL, 4) == 0);
   assert(td_extract_hit_docs(hits, ids, snips, 0) == 0);

   cJSON *obj = cJSON_CreateObject();
   assert(td_extract_hit_docs(obj, ids, snips, 4) == 0); /* non-array */
   cJSON_Delete(obj);
   cJSON_Delete(hits);
   printf("  ok: extract guards\n");
}

int main(void)
{
   printf("test_td_search_render:\n");
   test_render_multiple();
   test_render_empty();
   test_render_missing_fields();
   test_extract();
   test_extract_guards();
   printf("all td_search_render tests passed\n");
   return 0;
}
