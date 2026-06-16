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

   /* --- derived signals --- */
   int64_t fid2 = db2_code_index_file_upsert(pid, "src/signals.css", "2026-01-01T00:00:00Z");
   assert(fid2 >= 0);
   const char *sig = "#id .btn { color: red; }\n" /* line 1: spec (1,1,0) */
                     ".btn { color: blue; }\n"    /* line 2: later, less specific → loses color */
                     ".x { margin: 0; }\n"        /* line 3 */
                     ".y { margin: 0; }\n"        /* line 4: dup declaration margin:0 */
                     ".dup { color: red; }\n"     /* line 5: dup color:red value */
                     ".dup { color: green; }\n";  /* line 6: duplicate selector .dup */
   css_stylesheet_t *s2 = css_analyze(sig, strlen(sig));
   assert(s2 && db2_css_graph_replace(fid2, s2->rules, s2->rule_count) == 0);

   /* specificity conflict: later .btn (and .dup) can't override earlier #id .btn for color */
   css_spec_conflict_t conf[16];
   int nconf = db2_css_graph_specificity_conflicts("proj", conf, 16);
   /* #id .btn (winner) out-prioritises the 3 later, less-specific color rules
    * (.btn line2, .dup line5, .dup line6). */
   assert(nconf == 3);
   assert(strcmp(conf[0].winner_selector, "#id .btn") == 0);
   assert(strcmp(conf[0].loser_selector, ".btn") == 0);
   assert(strcmp(conf[0].property, "color") == 0);
   assert(conf[0].winner_line == 1 && conf[0].loser_line == 2);

   /* duplicate declarations: margin:0 (.x/.y) and color:red (#id .btn / .dup) */
   css_dup_decl_t dups[16];
   int ndup = db2_css_graph_duplicate_declarations("proj", dups, 16);
   assert(ndup == 2);

   /* duplicate selector: .dup twice */
   css_dup_selector_t dsel[16];
   int nds = db2_css_graph_duplicate_selectors("proj", dsel, 16);
   assert(nds == 1 && strcmp(dsel[0].selector, ".dup") == 0 && dsel[0].count == 2);

   css_stylesheet_free(s2);

   /* --- WP-D: component -> style join + dead rules (isolated project) --- */
   int64_t pd = db2_code_index_project_upsert("wpd", "/wpd");
   assert(pd >= 0);
   int64_t css_fid = db2_code_index_file_upsert(pd, "styles.css", "2026-01-01T00:00:00Z");
   const char *wcss = ".btn { color: red; }\n"
                      ".flex { display: flex; }\n"
                      ".dead { color: gray; }\n"         /* simple class, no component uses it */
                      ".btn.active { color: green; }\n"; /* compound, excluded from dead */
   css_stylesheet_t *w = css_analyze(wcss, strlen(wcss));
   assert(w && db2_css_graph_replace(css_fid, w->rules, w->rule_count) == 0);
   css_stylesheet_free(w);

   int64_t comp_fid = db2_code_index_file_upsert(pd, "Button.tsx", "2026-01-01T00:00:00Z");
   const char *tsx =
       "export const Button = () => <button className=\"btn flex missing\">x</button>;\n";
   char toks[64][CSS_CLASS_TOKEN_MAX];
   int nt = css_extract_class_tokens(tsx, strlen(tsx), toks, 64);
   assert(nt == 3); /* btn, flex, missing */
   assert(db2_css_component_resolve(comp_fid, toks, nt) == 0);

   /* "missing" resolves to no rule -> unresolved */
   css_unresolved_hit_t un[16];
   int nu = db2_css_component_unresolved("wpd", un, 16);
   assert(nu == 1 && strcmp(un[0].class_token, "missing") == 0);

   /* dead rules: .dead only (.btn/.flex are referenced; .btn.active is compound) */
   css_dead_rule_hit_t dr[16];
   int nd = db2_css_dead_rules("wpd", dr, 16);
   assert(nd == 1 && strcmp(dr[0].selector, ".dead") == 0);

   /* re-resolve is idempotent (delete-then-insert) */
   assert(db2_css_component_resolve(comp_fid, toks, nt) == 0);
   assert(db2_css_component_unresolved("wpd", un, 16) == 1);
   assert(db2_css_dead_rules("wpd", dr, 16) == 1);

   db2_test_shim_close();
   printf("css_graph: all tests passed\n");
   return 0;
}
