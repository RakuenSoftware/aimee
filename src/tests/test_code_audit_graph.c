/* test_code_audit_graph.c: unit tests for the pure graph-audit algorithms
 * (dead-export set-diff, import-cycle DFS). No DB. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "code_audit_graph.h"

static void test_dead_exports(void)
{
   const char *exports[] = {"export:p:Used", "export:p:Dead", "export:p:AlsoDead"};
   const char *imports[] = {"import:p:Used", "import:q:Other"};
   const char *out[8];
   int n = code_audit_dead_exports(exports, 3, imports, 2, out, 8);
   assert(n == 2);
   /* order preserved: Dead then AlsoDead */
   assert(strcmp(out[0], "export:p:Dead") == 0);
   assert(strcmp(out[1], "export:p:AlsoDead") == 0);

   /* all consumed -> none dead */
   const char *e2[] = {"export:p:A"};
   const char *i2[] = {"import:p:A"};
   assert(code_audit_dead_exports(e2, 1, i2, 1, out, 8) == 0);
   const char *r2[] = {"reference:p:A"};
   assert(code_audit_dead_exports(e2, 1, r2, 1, out, 8) == 0);

   /* no imports at all -> all dead, capped at max */
   assert(code_audit_dead_exports(exports, 3, NULL, 0, out, 1) == 1);
   printf("dead_exports OK\n");
}

static int cycle_mentions(char **cyc, int n, const char *a, const char *b)
{
   for (int i = 0; i < n; i++)
      if (strstr(cyc[i], a) && strstr(cyc[i], b))
         return 1;
   return 0;
}

static void test_find_cycles(void)
{
   char *out[16];

   /* a -> b -> a : one cycle involving a and b */
   audit_edge_t cyc2[] = {{"a", "b"}, {"b", "a"}};
   int n = code_audit_find_cycles(cyc2, 2, out, 16);
   assert(n >= 1);
   assert(cycle_mentions(out, n, "a", "b"));
   for (int i = 0; i < n; i++)
      free(out[i]);

   /* a -> b -> c : acyclic -> 0 */
   audit_edge_t dag[] = {{"a", "b"}, {"b", "c"}};
   assert(code_audit_find_cycles(dag, 2, out, 16) == 0);

   /* self-loop a -> a : a cycle */
   audit_edge_t self[] = {{"a", "a"}};
   n = code_audit_find_cycles(self, 1, out, 16);
   assert(n >= 1);
   for (int i = 0; i < n; i++)
      free(out[i]);

   /* 3-cycle a->b->c->a */
   audit_edge_t cyc3[] = {{"a", "b"}, {"b", "c"}, {"c", "a"}};
   n = code_audit_find_cycles(cyc3, 3, out, 16);
   assert(n >= 1);
   assert(cycle_mentions(out, n, "a", "c"));
   for (int i = 0; i < n; i++)
      free(out[i]);

   assert(code_audit_find_cycles(NULL, 0, out, 16) == 0);
   printf("find_cycles OK\n");
}

static void test_cycle_limit(void)
{
   char *out[4];
   audit_edge_t cycles[] = {
       {"a", "b"}, {"b", "a"}, {"c", "d"}, {"d", "c"}, {"e", "f"}, {"f", "e"},
   };
   int n = code_audit_find_cycles(cycles, 6, out, 2);
   assert(n == 2);
   for (int i = 0; i < n; i++)
      free(out[i]);
   printf("cycle_limit OK\n");
}

int main(void)
{
   printf("code_audit_graph: ");
   test_dead_exports();
   test_find_cycles();
   test_cycle_limit();
   printf("all tests passed\n");
   return 0;
}
