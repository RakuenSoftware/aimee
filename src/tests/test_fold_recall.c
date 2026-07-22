/* test_fold_recall.c: unit tests for fold recall (§4, P4). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../headers/dstr.h"
#include "fold_recall.h"

#define PASS(name) printf("  PASS: %s\n", name)

static int has(const char *hay, const char *needle)
{
   return strstr(hay, needle) != NULL;
}

static void test_add_dedup(void)
{
   fold_recall_index_t ix;
   fold_recall_index_init(&ix);
   fold_recall_index_add(&ix, "/src/foo.c");
   fold_recall_index_add(&ix, "/src/foo.c"); /* dup */
   fold_recall_index_add(&ix, "memory:abc");
   fold_recall_index_add(&ix, "");   /* ignored */
   fold_recall_index_add(&ix, NULL); /* ignored */
   assert(ix.count == 2);
   fold_recall_index_free(&ix);
   PASS("add_dedup");
}

static void test_detect_and_ttl(void)
{
   fold_recall_index_t ix;
   fold_recall_index_init(&ix);
   fold_recall_index_add(&ix, "/src/foo.c");
   fold_recall_index_add(&ix, "handle:xyz");

   /* turn 1: re-touch foo.c -> surfaced */
   dstr_t o1;
   dstr_init(&o1);
   size_t n1 = fold_recall_detect(&ix, "let me look at /src/foo.c again", 1, 4, &o1);
   assert(n1 == 1);
   assert(has(dstr_cstr(&o1), "/src/foo.c"));
   assert(has(dstr_cstr(&o1), "recall:"));
   dstr_free(&o1);

   /* turn 2: re-touch foo.c again within TTL(4) -> NOT surfaced (anti-thrash) */
   dstr_t o2;
   dstr_init(&o2);
   size_t n2 = fold_recall_detect(&ix, "still in /src/foo.c", 2, 4, &o2);
   assert(n2 == 0);
   dstr_free(&o2);

   /* turn 6: past TTL -> surfaced again */
   dstr_t o3;
   dstr_init(&o3);
   size_t n3 = fold_recall_detect(&ix, "back to /src/foo.c", 6, 4, &o3);
   assert(n3 == 1);
   dstr_free(&o3);

   /* a turn touching neither key surfaces nothing */
   dstr_t o4;
   dstr_init(&o4);
   size_t n4 = fold_recall_detect(&ix, "unrelated chatter", 7, 4, &o4);
   assert(n4 == 0);
   dstr_free(&o4);

   /* handle:xyz first touch surfaces */
   dstr_t o5;
   dstr_init(&o5);
   size_t n5 = fold_recall_detect(&ix, "see handle:xyz for details", 8, 4, &o5);
   assert(n5 == 1 && has(dstr_cstr(&o5), "handle:xyz"));
   dstr_free(&o5);

   fold_recall_index_free(&ix);
   PASS("detect_and_ttl");
}

static void test_empty_and_null(void)
{
   fold_recall_index_t ix;
   fold_recall_index_init(&ix);
   fold_recall_index_add(&ix, "/x");
   assert(fold_recall_detect(&ix, NULL, 1, 4, NULL) == 0);
   assert(fold_recall_detect(&ix, "", 1, 4, NULL) == 0);
   /* detect with NULL out still updates residency + counts */
   assert(fold_recall_detect(&ix, "touch /x here", 1, 4, NULL) == 1);
   assert(fold_recall_detect(&ix, "touch /x here", 2, 4, NULL) == 0); /* TTL */
   fold_recall_index_free(&ix);
   PASS("empty_and_null");
}

static void test_whole_token_match(void)
{
   /* substring collisions must NOT trigger recall: "/x" not in "/xyz",
    * "memory:42" not in "memory:420" — but exact tokens do match. */
   fold_recall_index_t ix;
   fold_recall_index_init(&ix);
   fold_recall_index_add(&ix, "/x");
   fold_recall_index_add(&ix, "memory:42");
   assert(fold_recall_detect(&ix, "editing /xyz now", 1, 4, NULL) == 0);
   assert(fold_recall_detect(&ix, "see memory:420 later", 1, 4, NULL) == 0);
   assert(fold_recall_detect(&ix, "open /x please", 1, 4, NULL) == 1); /* space-bounded token */
   assert(fold_recall_detect(&ix, "recall memory:42 now", 2, 4, NULL) == 1); /* space-bounded */
   fold_recall_index_free(&ix);
   PASS("whole_token_match");
}

int main(void)
{
   printf("fold_recall tests:\n");
   test_add_dedup();
   test_detect_and_ttl();
   test_empty_and_null();
   test_whole_token_match();
   printf("ALL PASS\n");
   return 0;
}
