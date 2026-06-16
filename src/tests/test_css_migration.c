/* test_css_migration.c: WP-F migration driver — enumerate from the component
 * join, coverage gate, state transitions, and the degraded-#2 rules doc. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "config.h"
#include "css_analyze.h"
#include "db2_test_shim.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "../db2/code_index.h"
#include "../db2/css_graph.h"
#include "../db2/css_migration.h"
#include "../db2/typed_facts.h"

static void test_gate(void)
{
   /* auto-accept only when oracle-equivalent AND coverage >= threshold */
   assert(css_migration_gate(2, 3, 1, 80) == CSS_MIGRATION_GATE_NEEDS_REVIEW); /* 66% < 80 */
   assert(css_migration_gate(2, 3, 1, 50) == CSS_MIGRATION_GATE_AUTO_ACCEPT);  /* 66% >= 50 */
   assert(css_migration_gate(3, 3, 0, 0) == CSS_MIGRATION_GATE_NEEDS_REVIEW);  /* oracle != yes */
   assert(css_migration_gate(0, 0, 1, 100) == CSS_MIGRATION_GATE_AUTO_ACCEPT); /* no tokens */
}

int main(void)
{
   test_gate();

   db2_test_shim_open();

   int64_t pid = db2_code_index_project_upsert("mig", "/mig");
   int64_t css_fid = db2_code_index_file_upsert(pid, "styles.css", "2026-01-01T00:00:00Z");
   const char *css = ".btn { color: red; }\n"
                     ".flex { display: flex; }\n"
                     ".card__title { font-weight: bold; }\n" /* BEM-ish */
                     ":root { --brand: #fff; }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss && db2_css_graph_replace(css_fid, ss->rules, ss->rule_count) == 0);
   css_stylesheet_free(ss);

   int64_t comp = db2_code_index_file_upsert(pid, "Button.tsx", "2026-01-01T00:00:00Z");
   const char *tsx = "<button className=\"btn flex missing\" />";
   char toks[64][CSS_CLASS_TOKEN_MAX];
   int nt = css_extract_class_tokens(tsx, strlen(tsx), toks, 64);
   assert(nt == 3);
   assert(db2_css_component_resolve(comp, toks, nt) == 0); /* btn,flex resolved; missing not */

   /* enumerate: one unit (Button.tsx), total=3 tokens, 2 resolved */
   int nu = db2_css_migration_enumerate("mig");
   assert(nu == 1);
   css_migration_unit_t units[16];
   int n = db2_css_migration_list("mig", NULL, units, 16);
   assert(n == 1);
   assert(strcmp(units[0].unit_path, "Button.tsx") == 0);
   assert(strcmp(units[0].state, "pending") == 0);
   assert(units[0].total_tokens == 3 && units[0].resolved_tokens == 2);

   /* gate on this unit's coverage (66%) */
   assert(css_migration_gate(units[0].resolved_tokens, units[0].total_tokens, 1, 90) ==
          CSS_MIGRATION_GATE_NEEDS_REVIEW);

   /* drive a state transition + oracle verdict */
   assert(db2_css_migration_set_state("mig", "Button.tsx", "verified", 1, "oracle equivalent",
                                      "2026-01-02T00:00:00Z") == 0);
   n = db2_css_migration_list("mig", "verified", units, 16);
   assert(n == 1 && units[0].oracle_equivalent == 1);
   assert(db2_css_migration_list("mig", "pending", units, 16) == 0);

   /* re-enumerate preserves the in-flight state but refreshes coverage */
   assert(db2_css_migration_enumerate("mig") == 1);
   n = db2_css_migration_list("mig", NULL, units, 16);
   assert(n == 1 && strcmp(units[0].state, "verified") == 0);

   /* degraded #2 rules doc derived from the exemplar style graph */
   char doc[4096];
   int dl = db2_css_migration_rules_doc("mig", doc, sizeof(doc));
   assert(dl > 0);
   assert(strstr(doc, "Convention Rules"));
   assert(strstr(doc, "BEM-like")); /* .card__title triggers the heuristic */
   assert(strstr(doc, "token"));

   /* --- #2-upgrade: typed convention facts (config-gated) --- */
   /* Off by default (no config) -> no-op, the degraded rules-doc is the spec. */
   assert(db2_css_migration_assert_conventions("mig", "2026-01-02T00:00:00Z") == 0);

   /* Enable both flags via an isolated config, then assert the conventions. */
   char home[512];
   snprintf(home, sizeof(home), "%s/aimee-mig-home-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   platform_setenv("HOME", home);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");
   char cfgdir[640];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", home);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[768];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *cf = fopen(cfgpath, "w");
   assert(cf);
   fputs("css_style_graph_enabled: true\ntyped_facts_enabled: true\n", cf);
   fclose(cf);

   /* mig has .card__title (BEM __) + :root --brand (custom property). */
   assert(db2_css_migration_assert_conventions("mig", "2026-01-02T00:00:00Z") == 2);

   typed_fact_t tf[8];
   int ntf = db2_typed_fact_recall("mig", "naming_convention", tf, 8);
   assert(ntf == 1 && strcmp(tf[0].object, "BEM") == 0);
   assert(strcmp(tf[0].source, "exemplar-scan") == 0);
   ntf = db2_typed_fact_recall("mig", "token_strategy", tf, 8);
   assert(ntf == 1 && strcmp(tf[0].object, "css-custom-properties") == 0);

   /* idempotent re-assert: same conventions, still 2 (UNCHANGED counts) */
   assert(db2_css_migration_assert_conventions("mig", "2026-01-03T00:00:00Z") == 2);
   assert(db2_typed_fact_recall("mig", "naming_convention", tf, 8) == 1);

   db2_test_shim_close();
   printf("css_migration: all tests passed\n");
   return 0;
}
