/* test_task_rail.c: unit tests for the portable plan FSM (fold §8, P5). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "task_rail.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_start_and_progress(void)
{
   const char *titles[] = {"design", "implement", "test"};
   task_rail_t r;
   task_rail_init(&r);
   assert(task_rail_start(&r, "ship the fold", titles, 3) == 0);
   assert(r.locked == 1 && r.count == 3);
   assert(task_rail_next(&r) == 0);
   assert(task_rail_done_count(&r) == 0);

   assert(task_rail_reserve(&r, 0) == 0);
   assert(r.steps[0].state == TASK_STEP_RESERVED);
   assert(task_rail_ack(&r, 0, "commit abc123") == 0);
   assert(r.steps[0].state == TASK_STEP_DONE);
   assert(strcmp(r.steps[0].evidence, "commit abc123") == 0);
   assert(task_rail_next(&r) == 1); /* cursor advanced */
   assert(task_rail_done_count(&r) == 1);

   task_rail_ack(&r, 1, NULL);
   task_rail_ack(&r, 2, NULL);
   assert(task_rail_next(&r) == -1); /* all done */
   assert(task_rail_done_count(&r) == 3);

   /* bad index/state */
   assert(task_rail_reserve(&r, 9) == -1);
   assert(task_rail_ack(&r, 9, NULL) == -1);

   task_rail_free(&r);
   PASS("start_and_progress");
}

static void test_serialize_restore_roundtrip(void)
{
   const char *titles[] = {"a", "b", "c"};
   task_rail_t r;
   task_rail_init(&r);
   task_rail_start(&r, "obj/with:coords #1", titles, 3);
   task_rail_ack(&r, 0, "ev0");
   task_rail_reserve(&r, 1);

   char *s1 = task_rail_serialize(&r);
   assert(s1);

   task_rail_t r2;
   task_rail_init(&r2);
   assert(task_rail_restore(&r2, s1) == 0);
   assert(r2.count == 3 && r2.locked == 1);
   assert(strcmp(r2.objective, "obj/with:coords #1") == 0);
   assert(r2.steps[0].state == TASK_STEP_DONE && strcmp(r2.steps[0].evidence, "ev0") == 0);
   assert(r2.steps[1].state == TASK_STEP_RESERVED);
   assert(r2.steps[2].state == TASK_STEP_PENDING);

   char *s2 = task_rail_serialize(&r2);
   assert(s2 && strcmp(s1, s2) == 0); /* deterministic round-trip */

   free(s1);
   free(s2);
   task_rail_free(&r);
   task_rail_free(&r2);
   PASS("serialize_restore_roundtrip");
}

static void test_bad_args(void)
{
   task_rail_t r;
   task_rail_init(&r);
   assert(task_rail_restore(&r, "{not json") == -1);
   assert(task_rail_serialize(NULL) == NULL);
   assert(task_rail_start(NULL, "x", NULL, 0) == -1);
   task_rail_free(&r);
   PASS("bad_args");
}

int main(void)
{
   printf("task_rail tests:\n");
   test_start_and_progress();
   test_serialize_restore_roundtrip();
   test_bad_args();
   printf("ALL PASS\n");
   return 0;
}
