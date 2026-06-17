/* test_css_insights.c: the read-only CSS analysis signals — !important audit,
 * high-specificity (id) selectors, unused custom properties, token candidates. */
#include "aimee.h"
#include "css_analyze.h"
#include "db2_test_shim.h"
#include "../db2/code_index.h"
#include "../db2/css_graph.h"
#include "../db2/css_insights.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   db2_test_shim_open();

   int64_t pid = db2_code_index_project_upsert("ins", "/ins");
   int64_t fid = db2_code_index_file_upsert(pid, "styles.css", "2026-01-01T00:00:00Z");
   const char *css = "#header { color: blue !important; }\n" /* id selector + !important */
                     ".y { display: none !important; }\n"    /* !important display */
                     ".btn { color: #ff0000; padding: 16px; }\n"
                     ".box { color: #ff0000; margin: 16px; }\n"
                     ".card { background: #ff0000; border-width: 16px; }\n"
                     ":root { --used: white; --unused: black; }\n"
                     ".x { color: var(--used); }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss && db2_css_graph_replace(fid, ss->rules, ss->rule_count) == 0);
   css_stylesheet_free(ss);

   /* !important audit: color (1) + display (1), most-frequent then alpha. */
   css_important_t imp[16];
   int ni = db2_css_important_audit("ins", imp, 16);
   assert(ni == 2);
   assert(strcmp(imp[0].property, "color") == 0 && imp[0].count == 1);
   assert(strcmp(imp[1].property, "display") == 0 && imp[1].count == 1);
   assert(imp[0].sample_file[0]); /* a sample file is reported */

   /* high-specificity: only #header has an id (spec_a > 0). */
   css_high_spec_t hs[16];
   int nh = db2_css_high_specificity("ins", hs, 16);
   assert(nh == 1);
   assert(strcmp(hs[0].selector, "#header") == 0 && hs[0].spec_a == 1);

   /* unused vars: --unused is never referenced; --used is (via var(--used)). */
   css_unused_var_t uv[16];
   int nu = db2_css_unused_custom_properties("ins", uv, 16);
   assert(nu == 1);
   assert(strcmp(uv[0].name, "--unused") == 0);

   /* token candidates (min 3): #ff0000 x3 (colour) and 16px x3 (length). Named
    * colours (white/black) and var() refs are not counted. */
   css_token_cand_t tc[16];
   int nt = db2_css_token_candidates("ins", 3, tc, 16);
   assert(nt == 2);
   /* both count 3, sorted by count desc then value: "#ff0000" < "16px". */
   assert(strcmp(tc[0].value, "#ff0000") == 0 && tc[0].count == 3 &&
          strcmp(tc[0].kind, "color") == 0);
   assert(strcmp(tc[1].value, "16px") == 0 && tc[1].count == 3 &&
          strcmp(tc[1].kind, "length") == 0);

   /* min_count gate: at 4, nothing qualifies (max repeat is 3). */
   assert(db2_css_token_candidates("ins", 4, tc, 16) == 0);

   db2_test_shim_close();
   printf("css_insights: all tests passed\n");
   return 0;
}
