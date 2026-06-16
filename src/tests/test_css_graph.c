/* test_css_graph.c: WP-B CSS style-graph persistence over the sqlite shim —
 * delete-then-insert refresh, selector/property queries, idempotent re-replace. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "css_analyze.h"
#include "db2_test_shim.h"
#include "../db2/code_index.h"
#include "../db2/css_graph.h"

static int count_rule_hits(const char *selector)
{
   css_rule_hit_t hits[32];
   int n = db2_css_graph_rules_by_selector(selector, hits, 32);
   return n;
}

int main(void)
{
   db2_test_shim_open();

   int64_t pid = db2_code_index_project_upsert("proj", "/root");
   assert(pid >= 0);
   int64_t fid = db2_code_index_file_upsert(pid, "src/app.css", "2026-01-01T00:00:00Z");
   assert(fid >= 0);

   const char *css = ".btn { color: red; padding: 8px !important; }\n"
                     "#main .btn, a.link { color: blue; }\n"
                     "@media (min-width: 600px) { .btn { color: green; } }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss && ss->rule_count == 4); /* .btn, (#main .btn), (a.link), .btn@media */

   assert(db2_css_graph_replace(fid, ss->rules, ss->rule_count) == 0);

   /* selector query: two `.btn` rules (top-level + @media) */
   css_rule_hit_t rhits[32];
   int nb = db2_css_graph_rules_by_selector(".btn", rhits, 32);
   assert(nb == 2);
   int media = 0, top = 0;
   for (int i = 0; i < nb; i++)
   {
      assert(strcmp(rhits[i].project, "proj") == 0);
      assert(strcmp(rhits[i].file_path, "src/app.css") == 0);
      assert(rhits[i].spec_b == 1 && rhits[i].spec_a == 0 && rhits[i].spec_c == 0);
      if (strstr(rhits[i].at_context, "@media"))
         media++;
      else
         top++;
   }
   assert(media == 1 && top == 1);

   /* compound selector specificity round-trips */
   int nm = db2_css_graph_rules_by_selector("#main .btn", rhits, 32);
   assert(nm == 1 && rhits[0].spec_a == 1 && rhits[0].spec_b == 1);

   /* declaration query by property, incl. !important flag */
   css_decl_hit_t dhits[32];
   int nc = db2_css_graph_declarations_by_property("color", dhits, 32);
   assert(nc == 4); /* every rule sets color */
   int npad = db2_css_graph_declarations_by_property("padding", dhits, 32);
   assert(npad == 1 && dhits[0].important == 1 && strcmp(dhits[0].value, "8px") == 0);

   /* idempotent re-replace: counts stay stable (delete-then-insert) */
   assert(db2_css_graph_replace(fid, ss->rules, ss->rule_count) == 0);
   assert(count_rule_hits(".btn") == 2);
   assert(db2_css_graph_declarations_by_property("color", dhits, 32) == 4);

   /* upsert_file convenience resolves the same file_id */
   assert(db2_css_graph_upsert_file("proj", "src/app.css", ss->rules, ss->rule_count) == 0);
   assert(count_rule_hits(".btn") == 2);

   /* replacing with an empty graph clears the file */
   assert(db2_css_graph_replace(fid, NULL, 0) == 0);
   assert(count_rule_hits(".btn") == 0);
   assert(db2_css_graph_declarations_by_property("color", dhits, 32) == 0);

   /* unknown file → upsert_file fails cleanly */
   assert(db2_css_graph_upsert_file("proj", "src/missing.css", ss->rules, ss->rule_count) == -1);

   css_stylesheet_free(ss);
   db2_test_shim_close();
   printf("css_graph: all tests passed\n");
   return 0;
}
