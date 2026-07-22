/* test_episode_seal.c: unit tests for sealed episodes (fold §5, P5). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "episode_seal.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_inventory_and_touch(void)
{
   episode_seal_t s;
   episode_seal_init(&s);
   episode_seal_set_conclusion(&s, "fixed the DNAT reconcile loop");
   episode_seal_add_file(&s, "src/db2/code_index.c");
   episode_seal_add_file(&s, "src/db2/code_index.c"); /* dedup */
   episode_seal_add_file(&s, "src/headers/code_span.h");
   episode_seal_add_file(&s, ""); /* ignored */
   assert(s.count == 2);

   /* auto-recall predicate */
   assert(episode_seal_touches(&s, "src/db2/code_index.c") == 1);
   assert(episode_seal_touches(&s, "src/headers/code_span.h") == 1);
   assert(episode_seal_touches(&s, "src/unrelated.c") == 0);

   episode_seal_free(&s);
   PASS("inventory_and_touch");
}

static void test_serialize_parse_roundtrip(void)
{
   episode_seal_t s;
   episode_seal_init(&s);
   episode_seal_set_conclusion(&s, "concluded: cache stays warm with freeze");
   episode_seal_add_file(&s, "src/context_fold.c");
   episode_seal_add_file(&s, "src/payload_rewrite.c");

   char *j1 = episode_seal_serialize(&s);
   assert(j1);

   episode_seal_t s2;
   episode_seal_init(&s2);
   assert(episode_seal_parse(&s2, j1) == 0);
   assert(strcmp(s2.conclusion, "concluded: cache stays warm with freeze") == 0);
   assert(s2.count == 2);
   assert(episode_seal_touches(&s2, "src/context_fold.c") == 1);

   char *j2 = episode_seal_serialize(&s2);
   assert(j2 && strcmp(j1, j2) == 0); /* deterministic round-trip */

   free(j1);
   free(j2);
   episode_seal_free(&s);
   episode_seal_free(&s2);
   PASS("serialize_parse_roundtrip");
}

static void test_bad_args(void)
{
   episode_seal_t s;
   episode_seal_init(&s);
   assert(episode_seal_parse(&s, "{bad") == -1);
   assert(episode_seal_serialize(NULL) == NULL);
   assert(episode_seal_touches(NULL, "x") == 0);
   episode_seal_free(&s);
   PASS("bad_args");
}

int main(void)
{
   printf("episode_seal tests:\n");
   test_inventory_and_touch();
   test_serialize_parse_roundtrip();
   test_bad_args();
   printf("ALL PASS\n");
   return 0;
}
