/* test_css_oracle.c: WP-E interim static oracle — cascade resolution, equivalence,
 * change/add/remove diffs, whitespace normalization, conservative shorthand handling. */
#include "css_analyze.h"
#include "css_oracle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Parse a single rule body and hand back its declarations (owns the stylesheet
 * via the returned handle; caller frees with css_stylesheet_free). */
static css_stylesheet_t *decls_of(const char *body, const css_declaration_t **out, int *n)
{
   char css[2048];
   snprintf(css, sizeof(css), ".x { %s }", body);
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss && ss->rule_count == 1);
   *out = ss->rules[0].decls;
   *n = ss->rules[0].decl_count;
   return ss;
}

static const css_oracle_diff_t *find_diff(const css_oracle_result_t *r, const char *prop)
{
   for (int i = 0; i < r->diff_count; i++)
      if (strcmp(r->diffs[i].property, prop) == 0)
         return &r->diffs[i];
   return NULL;
}

static void test_equivalent_reorder_and_whitespace(void)
{
   const css_declaration_t *b, *a;
   int nb, na;
   css_stylesheet_t *sb = decls_of("color: red; margin: 0  auto;", &b, &nb);
   css_stylesheet_t *sa = decls_of("margin: 0 auto; color: red;", &a, &na);
   css_oracle_result_t *r = css_oracle_compare(b, nb, a, na);
   assert(r);
   assert(r->equivalent == 1 && r->diff_count == 0);
   assert(r->limitation && strstr(r->limitation, "STATIC"));
   css_oracle_result_free(r);
   css_stylesheet_free(sb);
   css_stylesheet_free(sa);
}

static void test_changed_added_removed(void)
{
   const css_declaration_t *b, *a;
   int nb, na;
   css_stylesheet_t *sb = decls_of("color: red; padding: 4px;", &b, &nb);
   css_stylesheet_t *sa = decls_of("color: blue; gap: 8px;", &a, &na);
   css_oracle_result_t *r = css_oracle_compare(b, nb, a, na);
   assert(r && r->equivalent == 0);
   const css_oracle_diff_t *col = find_diff(r, "color");
   assert(col && col->kind == CSS_ORACLE_CHANGED);
   assert(strcmp(col->before_value, "red") == 0 && strcmp(col->after_value, "blue") == 0);
   const css_oracle_diff_t *pad = find_diff(r, "padding");
   assert(pad && pad->kind == CSS_ORACLE_REMOVED);
   const css_oracle_diff_t *gap = find_diff(r, "gap");
   assert(gap && gap->kind == CSS_ORACLE_ADDED);
   css_oracle_result_free(r);
   css_stylesheet_free(sb);
   css_stylesheet_free(sa);
}

static void test_cascade_resolution(void)
{
   const css_declaration_t *b, *a;
   int nb, na;
   /* before: later non-important wins -> color:green; important beats later -> z-index:1 */
   css_stylesheet_t *sb =
       decls_of("color: red; color: green; z-index: 1 !important; z-index: 9;", &b, &nb);
   /* after resolves to the same effective set written plainly */
   css_stylesheet_t *sa = decls_of("color: green; z-index: 1 !important;", &a, &na);
   css_oracle_result_t *r = css_oracle_compare(b, nb, a, na);
   assert(r && r->equivalent == 1); /* cascade collapse makes them equivalent */
   css_oracle_result_free(r);
   css_stylesheet_free(sb);
   css_stylesheet_free(sa);

   /* !important mismatch alone is a difference */
   css_stylesheet_t *s1 = decls_of("color: red;", &b, &nb);
   css_stylesheet_t *s2 = decls_of("color: red !important;", &a, &na);
   r = css_oracle_compare(b, nb, a, na);
   assert(r && r->equivalent == 0);
   const css_oracle_diff_t *c = find_diff(r, "color");
   assert(c && c->kind == CSS_ORACLE_CHANGED && c->before_important == 0 &&
          c->after_important == 1);
   css_oracle_result_free(r);
   css_stylesheet_free(s1);
   css_stylesheet_free(s2);
}

static void test_shorthand_not_expanded_conservative(void)
{
   const css_declaration_t *b, *a;
   int nb, na;
   /* The static oracle does NOT expand shorthands: margin vs margin-top/... is
    * reported as a difference, never a false "equivalent" (no silent caps). */
   css_stylesheet_t *sb = decls_of("margin: 0;", &b, &nb);
   css_stylesheet_t *sa =
       decls_of("margin-top: 0; margin-right: 0; margin-bottom: 0; margin-left: 0;", &a, &na);
   css_oracle_result_t *r = css_oracle_compare(b, nb, a, na);
   assert(r && r->equivalent == 0);
   assert(find_diff(r, "margin") && find_diff(r, "margin")->kind == CSS_ORACLE_REMOVED);
   assert(find_diff(r, "margin-top") && find_diff(r, "margin-top")->kind == CSS_ORACLE_ADDED);
   css_oracle_result_free(r);
   css_stylesheet_free(sb);
   css_stylesheet_free(sa);
}

static void test_empty_sets(void)
{
   css_oracle_result_t *r = css_oracle_compare(NULL, 0, NULL, 0);
   assert(r && r->equivalent == 1 && r->diff_count == 0);
   css_oracle_result_free(r);
}

int main(void)
{
   test_equivalent_reorder_and_whitespace();
   test_changed_added_removed();
   test_cascade_resolution();
   test_shorthand_not_expanded_conservative();
   test_empty_sets();
   printf("css_oracle: all tests passed\n");
   return 0;
}
