/* test_css_treesitter.c: the tree-sitter CSS front-end against the scanner it
 * replaces.
 *
 * css_analyze prefers the grammar and falls back to the hand-rolled scanner, so
 * the property that matters is that the two agree: a build with tree-sitter and
 * a build without it must read the same stylesheet the same way. Anywhere they
 * do not, the fallback is not a fallback but a second answer.
 *
 * Opt-in only (links the fetched runtime + the vendored grammar), like
 * unit-test-code-treesitter. */
#include "css_analyze.h"
#include "css_treesitter.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures;

/* css_analyze's scanner is reached by asking the front-end for nothing, which
 * is what the stub build does. There is no way to call it directly from here,
 * so the scanner's answers are written out: they are what
 * unit-test-css-analyze already asserts, restated as the thing the grammar has
 * to match. */
typedef struct
{
   const char *selector;
   int spec_a, spec_b, spec_c;
   int uncertain;
   const char *at_context;
   int line;
   int decl_count;
} expected_rule_t;

static void check_sheet(const char *what, const char *source, const expected_rule_t *expected,
                        int expected_count)
{
   css_stylesheet_t *sheet = css_treesitter_analyze(source, strlen(source));
   if (!sheet)
   {
      fprintf(stderr, "%s: the front-end returned nothing\n", what);
      failures++;
      return;
   }
   if (sheet->rule_count != expected_count)
   {
      fprintf(stderr, "%s: %d rules, expected %d\n", what, sheet->rule_count, expected_count);
      for (int i = 0; i < sheet->rule_count; i++)
         fprintf(stderr, "    [%d] '%s' (%d,%d,%d) u=%d at='%s' line=%d decls=%d\n", i,
                 sheet->rules[i].selector, sheet->rules[i].spec_a, sheet->rules[i].spec_b,
                 sheet->rules[i].spec_c, sheet->rules[i].specificity_uncertain,
                 sheet->rules[i].at_context, sheet->rules[i].line, sheet->rules[i].decl_count);
      failures++;
      css_stylesheet_free(sheet);
      return;
   }
   for (int i = 0; i < expected_count; i++)
   {
      const css_rule_t *got = &sheet->rules[i];
      const expected_rule_t *want = &expected[i];
      if (strcmp(got->selector, want->selector) != 0 || got->spec_a != want->spec_a ||
          got->spec_b != want->spec_b || got->spec_c != want->spec_c ||
          got->specificity_uncertain != want->uncertain ||
          strcmp(got->at_context, want->at_context) != 0 || got->line != want->line ||
          got->decl_count != want->decl_count)
      {
         fprintf(stderr,
                 "%s [%d]: got '%s' (%d,%d,%d) u=%d at='%s' line=%d decls=%d\n"
                 "%*s expected '%s' (%d,%d,%d) u=%d at='%s' line=%d decls=%d\n",
                 what, i, got->selector, got->spec_a, got->spec_b, got->spec_c,
                 got->specificity_uncertain, got->at_context, got->line, got->decl_count,
                 (int)strlen(what) + 5, "", want->selector, want->spec_a, want->spec_b,
                 want->spec_c, want->uncertain, want->at_context, want->line, want->decl_count);
         failures++;
      }
   }
   css_stylesheet_free(sheet);
}

/* One selector's specificity, read back from a stylesheet of one rule. */
static void spec(const char *selector, int a, int b, int c, int uncertain)
{
   char source[512];
   snprintf(source, sizeof(source), "%s { color: red }\n", selector);
   css_stylesheet_t *sheet = css_treesitter_analyze(source, strlen(source));
   if (!sheet || sheet->rule_count != 1)
   {
      fprintf(stderr, "spec('%s'): parsed %d rules\n", selector, sheet ? sheet->rule_count : -1);
      failures++;
      css_stylesheet_free(sheet);
      return;
   }
   const css_rule_t *rule = &sheet->rules[0];
   if (rule->spec_a != a || rule->spec_b != b || rule->spec_c != c ||
       rule->specificity_uncertain != uncertain)
   {
      fprintf(stderr, "spec('%s') = (%d,%d,%d) u=%d, expected (%d,%d,%d) u=%d\n", selector,
              rule->spec_a, rule->spec_b, rule->spec_c, rule->specificity_uncertain, a, b, c,
              uncertain);
      failures++;
   }
   css_stylesheet_free(sheet);
}

/* The specificity cases unit-test-css-analyze asserts of the scanner. The
 * grammar has to reach the same numbers by a different route. */
static void test_specificity_matches_scanner(void)
{
   spec("#id", 1, 0, 0, 0);
   spec(".cls", 0, 1, 0, 0);
   spec("div", 0, 0, 1, 0);
   spec("*", 0, 0, 0, 0);
   spec("#a .b div", 1, 1, 1, 0);
   spec("ul li", 0, 0, 2, 0);
   spec("a:hover", 0, 1, 1, 0);
   spec("div::before", 0, 0, 2, 0);
   spec("[data-x]", 0, 1, 0, 0);
   spec(".a > .b + .c ~ .d", 0, 4, 0, 0);
   spec(".a.b.c", 0, 3, 0, 0);
   spec(":where(#x, .y)", 0, 0, 0, 0);
   spec(":is(#x, .y)", 1, 0, 0, 0);
   spec(":not(.a, #b)", 1, 0, 0, 0);
   spec("a:is(.x):hover", 0, 2, 1, 0);
   spec(":nth-child(2n+1)", 0, 1, 0, 0);
   spec("&.x", 0, 1, 0, 1);
   printf("  specificity_matches_scanner: %s\n", failures ? "FAILED" : "ok");
}

static void test_rules_and_declarations(void)
{
   static const expected_rule_t expected[] = {
       {".a", 0, 1, 0, 0, "", 1, 2},
       {"#b", 1, 0, 0, 0, "", 1, 2},
       {"div", 0, 0, 1, 0, "", 4, 1},
   };
   check_sheet("rules_and_declarations",
               ".a, #b { color: red; margin: 0 }\n"
               "\n"
               "\n"
               "div { padding: 1px !important }\n",
               expected, 3);
   printf("  rules_and_declarations: ok\n");
}

static void test_at_context(void)
{
   static const expected_rule_t expected[] = {
       {".plain", 0, 1, 0, 0, "", 1, 1},
       {".inner", 0, 1, 0, 0, "@media (min-width: 700px)", 3, 1},
   };
   check_sheet("at_context",
               ".plain { color: red }\n"
               "@media (min-width: 700px) {\n"
               "  .inner { color: blue }\n"
               "}\n",
               expected, 2);
   printf("  at_context: ok\n");
}

static void test_important(void)
{
   css_stylesheet_t *sheet =
       css_treesitter_analyze(".a { color: red !important; margin: 0 }\n", 39);
   assert(sheet);
   assert(sheet->rule_count == 1);
   assert(sheet->rules[0].decl_count == 2);
   assert(sheet->rules[0].decls[0].important == 1);
   assert(sheet->rules[0].decls[1].important == 0);
   assert(strcmp(sheet->rules[0].decls[0].property, "color") == 0);
   assert(strcmp(sheet->rules[0].decls[1].property, "margin") == 0);
   css_stylesheet_free(sheet);
   printf("  important: ok\n");
}

/* The analyzer's contract is that malformed input never crashes and is bounded.
 * The grammar has an error recovery of its own; what matters here is that it
 * returns rather than faulting, and that css_analyze still answers. */
static void test_malformed_is_bounded(void)
{
   static const char *const malformed[] = {
       "",
       "{",
       "}",
       "a {",
       "a { color",
       "a { color: }",
       "@media {",
       "/* unterminated",
       "a[data-x=\"}\"] { color: red }",
       "@font-face { font-family: x }",
   };
   for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
   {
      css_stylesheet_t *sheet = css_treesitter_analyze(malformed[i], strlen(malformed[i]));
      css_stylesheet_free(sheet);
      /* css_analyze must answer whichever path it took. */
      css_stylesheet_t *either = css_analyze(malformed[i], strlen(malformed[i]));
      assert(either != NULL);
      css_stylesheet_free(either);
   }
   printf("  malformed_is_bounded: ok\n");
}

int main(void)
{
   printf("css_treesitter:\n");
   test_specificity_matches_scanner();
   test_rules_and_declarations();
   test_at_context();
   test_important();
   test_malformed_is_bounded();
   if (failures)
   {
      fprintf(stderr, "css_treesitter: %d check(s) failed\n", failures);
      return 1;
   }
   printf("All css_treesitter tests passed.\n");
   return 0;
}
