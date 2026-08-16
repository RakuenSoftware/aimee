#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "tasks_compose.h"

static void setup(void)
{
   /* tasks and decision_log both round-trip through this test. db1 is
    * also initialised for decisions used by checkpoint composition. */
   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();
}

static void teardown(void)
{
   db2_test_shim_close();
   db1_shutdown();
}

static void test_create_task(void)
{
   setup();
   aimee_task_t t;
   int rc = db2_task_create("Implement auth", "s1", 0, &t);
   assert(rc == 0);
   assert(strcmp(t.title, "Implement auth") == 0);
   assert(strcmp(t.state, TASK_TODO) == 0);
   teardown();
}

static void test_update_state(void)
{
   setup();
   aimee_task_t t;
   db2_task_create("Test task", "s1", 0, &t);
   db2_task_update_state(t.id, TASK_IN_PROGRESS);

   aimee_task_t updated;
   db2_task_get(t.id, &updated);
   assert(strcmp(updated.state, TASK_IN_PROGRESS) == 0);
   teardown();
}

static void test_subtasks(void)
{
   setup();
   aimee_task_t parent, child;
   db2_task_create("Parent", "s1", 0, &parent);
   db2_task_create("Child", "s1", parent.id, &child);

   aimee_task_t subs[10];
   int n = db2_task_get_subtasks(parent.id, subs, 10);
   assert(n == 1);
   assert(strcmp(subs[0].title, "Child") == 0);
   teardown();
}

static void test_task_edges(void)
{
   setup();
   aimee_task_t t1, t2;
   db2_task_create("Design", "s1", 0, &t1);
   db2_task_create("Implement", "s1", 0, &t2);

   int rc = db2_task_add_edge(t2.id, t1.id, "depends_on");
   assert(rc == 0);

   task_edge_t edges[10];
   int n = db2_task_get_edges(t2.id, edges, 10);
   assert(n == 1);
   assert(strcmp(edges[0].relation, "depends_on") == 0);
   teardown();
}

static void test_decision_log(void)
{
   setup();
   assert(db1_init(":memory:") == 0);
   db2_decision_log_row_t d;
   int rc = db2_decision_log_insert(0, "A, B", "A", "simpler", "API stable", NULL, &d);
   assert(rc == 0);
   assert(strcmp(d.chosen, "A") == 0);

   assert(db2_decision_log_set_outcome(d.id, "success") == 0);
   db2_decision_log_row_t updated;
   assert(db2_decision_log_get(d.id, &updated) == 0);
   assert(strcmp(updated.outcome, "success") == 0);
   db1_shutdown();
   teardown();
}

static void test_checkpoint(void)
{
   setup();
   db1_checkpoint_t cp;
   int rc = tasks_checkpoint_create("Before refactor", "s1", 0, &cp);
   assert(rc == 0);
   assert(strcmp(cp.label, "Before refactor") == 0);
   assert(strlen(cp.snapshot) > 0);

   db1_checkpoint_t list[10];
   int n = db1_checkpoint_list(10, list, 10);
   assert(n == 1);

   db1_checkpoint_delete(cp.id);
   n = db1_checkpoint_list(10, list, 10);
   assert(n == 0);
   teardown();
}

static void test_active_task(void)
{
   setup();
   aimee_task_t t;
   db2_task_create("Active task", "sess-1", 0, &t);
   db2_task_update_state(t.id, TASK_IN_PROGRESS);

   int64_t active = db2_task_get_active("sess-1");
   assert(active == t.id);

   int64_t none = db2_task_get_active("other-session");
   assert(none == 0);
   teardown();
}

int main(void)
{
   test_create_task();
   test_update_state();
   test_subtasks();
   test_task_edges();
   test_decision_log();
   test_checkpoint();
   test_active_task();
   printf("tasks: all tests passed\n");
   return 0;
}
