/* Behavior parity for descriptor-owned DB2 code-audit graph algorithms. */
#include "code_audit_graph.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db2_support_code_audit_dead_exports(const char *const *exports, int n_exports,
                                        const char *const *imports, int n_imports, const char **out,
                                        int max);
int db2_support_code_audit_find_cycles(const audit_edge_t *edges, int n_edges, char **out, int max);

_Static_assert(sizeof(audit_edge_t) == sizeof(const char *) * 2, "audit edge ABI drifted");
_Static_assert(offsetof(audit_edge_t, from) == 0, "audit edge source offset drifted");
_Static_assert(offsetof(audit_edge_t, to) == sizeof(const char *),
               "audit edge target offset drifted");

static void assert_dead_exports(const char *const *exports, int n_exports,
                                const char *const *imports, int n_imports, int max)
{
   const char *legacy[32];
   const char *support[32];
   for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++)
      legacy[i] = support[i] = (const char *)(size_t)1;
   int legacy_count = code_audit_dead_exports(exports, n_exports, imports, n_imports, legacy, max);
   int support_count =
       db2_support_code_audit_dead_exports(exports, n_exports, imports, n_imports, support, max);
   assert(legacy_count == support_count);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
}

static void test_dead_exports(void)
{
   static const char *exports[] = {"export:p:a", "p:b",           NULL,        "export:p:c",
                                   "export:",    "reference:p:d", "export:p:a"};
   static const char *imports[] = {"import:p:a", "reference:p:c", NULL,
                                   "p:d",        "import:",       "export:p:b"};
   for (int n_exports = -1; n_exports <= (int)(sizeof(exports) / sizeof(exports[0])); n_exports++)
      for (int n_imports = -1; n_imports <= (int)(sizeof(imports) / sizeof(imports[0]));
           n_imports++)
         for (int max = -1; max <= 12; max++)
            assert_dead_exports(exports, n_exports, imports, n_imports, max);

   assert_dead_exports(NULL, 0, NULL, 0, 0);
}

static void free_cycles(char **cycles, int count)
{
   for (int i = 0; i < count; i++)
      free(cycles[i]);
}

static void assert_cycles(const audit_edge_t *edges, int n_edges, int max)
{
   char *legacy[32] = {0};
   char *support[32] = {0};
   int legacy_count = code_audit_find_cycles(edges, n_edges, legacy, max);
   int support_count = db2_support_code_audit_find_cycles(edges, n_edges, support, max);
   assert(legacy_count == support_count);
   for (int i = 0; i < legacy_count; i++)
      assert(strcmp(legacy[i], support[i]) == 0);
   for (int i = legacy_count; i < 32; i++)
      assert(legacy[i] == NULL && support[i] == NULL);
   free_cycles(legacy, legacy_count);
   free_cycles(support, support_count);
}

static void test_cycle_fixtures(void)
{
   static const audit_edge_t self[] = {{"a", "a"}};
   static const audit_edge_t pair[] = {{"a", "b"}, {"b", "a"}};
   static const audit_edge_t triple[] = {{"a", "b"}, {"b", "c"}, {"c", "a"}};
   static const audit_edge_t dag[] = {{"a", "b"}, {"b", "c"}, {"a", "c"}};
   static const audit_edge_t mixed[] = {{NULL, "a"}, {"a", NULL}, {"a", "b"},
                                        {"b", "a"},  {"x", "y"},  {"y", "z"},
                                        {"z", "x"},  {"a", "b"},  {"b", "a"}};
   static const audit_edge_t overlapping[] = {
       {"a", "b"}, {"b", "c"}, {"c", "a"}, {"b", "d"}, {"d", "b"}};
   const audit_edge_t *fixtures[] = {self, pair, triple, dag, mixed, overlapping};
   const int counts[] = {1, 2, 3, 3, 9, 5};
   for (size_t fixture = 0; fixture < sizeof(fixtures) / sizeof(fixtures[0]); fixture++)
      for (int max = -1; max <= 12; max++)
         assert_cycles(fixtures[fixture], counts[fixture], max);
   assert_cycles(NULL, 0, 0);
   assert_cycles(NULL, 1, 1);
}

static void test_generated_graphs(void)
{
   enum
   {
      NODES = 64
   };
   char names[NODES][16];
   audit_edge_t edges[NODES + 8];
   for (int i = 0; i < NODES; i++)
   {
      snprintf(names[i], sizeof(names[i]), "node-%02d", i);
      edges[i].from = names[i];
      edges[i].to = names[(i + 1) % NODES];
   }
   for (int i = 0; i < 8; i++)
   {
      edges[NODES + i].from = names[i * 4];
      edges[NODES + i].to = names[i * 4 + 2];
   }
   for (int max = 1; max <= 16; max++)
      assert_cycles(edges, NODES + 8, max);
}

int main(void)
{
   test_dead_exports();
   test_cycle_fixtures();
   test_generated_graphs();
   return 0;
}
