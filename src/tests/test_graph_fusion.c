/* test_graph_fusion.c: DB2 graph provenance read coverage. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "platform_test_util.h"
#include "modules/db2/c/entity_edges.h"

static char g_db_path[512];

static void setup(void)
{
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-gf-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(g_db_path, sizeof(g_db_path), "aim");
   assert(fd >= 0);
   close(fd);
   db2_test_shim_open_path(g_db_path);
}

static void teardown(void)
{
   db2_test_shim_close();
   platform_test_remove_sqlite(g_db_path);
   g_db_path[0] = '\0';
}

static void test_explain_read_provenance(void)
{
   setup();
   int added = 0;
   db2_entity_edge_upsert("file:p:a.c", "defines", "symbol:p:foo", 0, 5, 0, 1, &added);
   db2_entity_edge_explain_t rows[16];
   int n = db2_entity_edge_explain_by_entity("symbol:p:foo", rows, 16);
   assert(n >= 1);
   int seen = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].relation, "defines") == 0 && strcmp(rows[i].source, "file:p:a.c") == 0)
         seen = 1;
   assert(seen);
   teardown();
}

int main(void)
{
   test_explain_read_provenance();
   printf("graph provenance: all tests passed\n");
   return 0;
}
