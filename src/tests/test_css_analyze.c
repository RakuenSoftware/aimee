/* test_css_analyze.c: WP-A CSS analyzer — specificity math, rule/declaration
 * parsing, at-rule context, !important, and malformed-input resilience. */
#include "css_analyze.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void spec(const char *sel, int a, int b, int c, int uncertain)
{
   int ra, rb, rc, ru;
   css_selector_specificity(sel, &ra, &rb, &rc, &ru);
   if (ra != a || rb != b || rc != c || ru != uncertain)
      fprintf(stderr, "spec('%s') = (%d,%d,%d) u=%d, expected (%d,%d,%d) u=%d\n", sel, ra, rb, rc,
              ru, a, b, c, uncertain);
   assert(ra == a && rb == b && rc == c && ru == uncertain);
}

static void test_specificity(void)
{
   spec("#id", 1, 0, 0, 0);
   spec(".cls", 0, 1, 0, 0);
   spec("div", 0, 0, 1, 0);
   spec("*", 0, 0, 0, 0);
   spec("#a .b div", 1, 1, 1, 0);
   spec("ul li", 0, 0, 2, 0);
   spec("a:hover", 0, 1, 1, 0);           /* type + pseudo-class */
   spec("div::before", 0, 0, 2, 0);       /* type + pseudo-element */
   spec("[data-x]", 0, 1, 0, 0);          /* attribute */
   spec("a[href='x']", 0, 1, 1, 0);       /* type + attr (quoted, contains no specials) */
   spec(".a > .b + .c ~ .d", 0, 4, 0, 0); /* combinators don't add */
   spec(".a.b.c", 0, 3, 0, 0);            /* compound */
   /* functional pseudo-classes */
   spec(":where(#x, .y)", 0, 0, 0, 0);                /* :where contributes 0 */
   spec(":is(#x, .y)", 1, 0, 0, 0);                   /* :is = max of args = #x */
   spec(":not(.a, #b)", 1, 0, 0, 0);                  /* :not = max of args = #b */
   spec("a:is(.x):hover", 0, 2, 1, 0);                /* type + :is(.x)=class + :hover=class */
   spec(":nth-child(2n+1)", 0, 1, 0, 0);              /* nth-* is one pseudo-class */
   spec("li:nth-child(odd of .visible)", 0, 1, 1, 1); /* "of S" → uncertain */
   spec("&.x", 0, 1, 0, 1);                           /* nesting selector → uncertain */
}

static const css_rule_t *find_rule(const css_stylesheet_t *ss, const char *sel)
{
   for (int i = 0; i < ss->rule_count; i++)
      if (strcmp(ss->rules[i].selector, sel) == 0)
         return &ss->rules[i];
   return NULL;
}

static void test_basic_parse(void)
{
   const char *css = ".btn {\n"
                     "  color: red;\n"
                     "  margin: 0 auto !important;\n"
                     "}\n"
                     "#main, .side > a { display: block; }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss);
   /* .btn + (#main) + (.side > a) = 3 rules */
   assert(ss->rule_count == 3);

   const css_rule_t *btn = find_rule(ss, ".btn");
   assert(btn);
   assert(btn->spec_a == 0 && btn->spec_b == 1 && btn->spec_c == 0);
   assert(btn->line == 1);
   assert(btn->decl_count == 2);
   assert(strcmp(btn->decls[0].property, "color") == 0);
   assert(strcmp(btn->decls[0].value, "red") == 0);
   assert(btn->decls[0].important == 0);
   assert(strcmp(btn->decls[1].property, "margin") == 0);
   assert(strcmp(btn->decls[1].value, "0 auto") == 0);
   assert(btn->decls[1].important == 1);

   /* comma list split into two distinct rules with their own specificity */
   const css_rule_t *main_r = find_rule(ss, "#main");
   const css_rule_t *side_r = find_rule(ss, ".side > a");
   assert(main_r && side_r);
   assert(main_r->spec_a == 1);
   assert(side_r->spec_b == 1 && side_r->spec_c == 1);
   assert(main_r->decl_count == 1 && side_r->decl_count == 1);
   assert(strcmp(side_r->decls[0].property, "display") == 0);

   css_stylesheet_free(ss);
}

static void test_at_context(void)
{
   const char *css =
       "@media (min-width: 600px) {\n"
       "  .col { width: 50%; }\n"
       "}\n"
       ".col { width: 100%; }\n"
       "@font-face { font-family: Foo; src: url(x.woff); }\n"
       "@keyframes spin { from { transform: rotate(0); } to { transform: rotate(1turn); } }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss);

   /* two `.col` rules: one inside @media, one at top level */
   int in_media = 0, top = 0;
   for (int i = 0; i < ss->rule_count; i++)
   {
      if (strcmp(ss->rules[i].selector, ".col") == 0)
      {
         if (strstr(ss->rules[i].at_context, "@media"))
            in_media++;
         else if (ss->rules[i].at_context[0] == '\0')
            top++;
      }
   }
   assert(in_media == 1 && top == 1);

   /* @font-face captured as a synthetic rule with declarations */
   const css_rule_t *ff = find_rule(ss, "@font-face");
   assert(ff && ff->decl_count == 2 && ff->specificity_uncertain == 1);

   /* @keyframes inner blocks are skipped, not parsed as rules */
   for (int i = 0; i < ss->rule_count; i++)
      assert(strcmp(ss->rules[i].selector, "from") != 0 &&
             strcmp(ss->rules[i].selector, "to") != 0);

   css_stylesheet_free(ss);
}

static void test_malformed_resilience(void)
{
   /* Unterminated rule, unbalanced braces/parens, stray chars — must not crash. */
   const char *bad[] = {".x { color: red",            /* missing close brace + ; */
                        "}}}{{{ .a { b",              /* garbage braces */
                        ".y:is(.a, { color: blue; }", /* unbalanced paren in selector */
                        "/* unclosed comment .z { color: x;",
                        "",
                        "@media",
                        ".w { --v: ; ; ; }",
                        NULL};
   for (int i = 0; bad[i]; i++)
   {
      css_stylesheet_t *ss = css_analyze(bad[i], strlen(bad[i]));
      assert(ss); /* returns a (possibly empty) graph, never crashes */
      css_stylesheet_free(ss);
   }
   /* NULL input → NULL, no crash */
   assert(css_analyze(NULL, 0) == NULL);
}

static void test_custom_properties_and_important(void)
{
   const char *css = ":root { --brand: #fff; --gap: 8px; }\n"
                     ".a { color: var(--brand) !important; }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss);
   const css_rule_t *root = find_rule(ss, ":root");
   assert(root && root->decl_count == 2);
   assert(strcmp(root->decls[0].property, "--brand") == 0);
   assert(strcmp(root->decls[0].value, "#fff") == 0);
   const css_rule_t *a = find_rule(ss, ".a");
   assert(a && a->decl_count == 1);
   assert(a->decls[0].important == 1);
   assert(strcmp(a->decls[0].value, "var(--brand)") == 0);
   css_stylesheet_free(ss);
}

/* Structural characters inside strings / url() must not be mis-read as syntax. */
static void test_strings_and_urls(void)
{
   const char *css = ".a {\n"
                     "  background: url(data:image/svg+xml;base64,PHN2Zz48L3N2Zz4=) no-repeat;\n"
                     "  color: red;\n"
                     "}\n"
                     ".b::before { content: \"};{\"; display: block; }\n";
   css_stylesheet_t *ss = css_analyze(css, strlen(css));
   assert(ss);

   const css_rule_t *a = find_rule(ss, ".a");
   assert(a && a->decl_count == 2); /* the ';' inside url() did not split */
   assert(strcmp(a->decls[0].property, "background") == 0);
   assert(strstr(a->decls[0].value, "base64,PHN2Zz48L3N2Zz4=") != NULL);
   assert(strcmp(a->decls[1].property, "color") == 0);

   const css_rule_t *b = find_rule(ss, ".b::before");
   assert(b && b->decl_count == 2); /* the '}' and ';' inside content did not break the block */
   assert(strcmp(b->decls[0].property, "content") == 0);
   assert(strcmp(b->decls[1].property, "display") == 0);
   assert(b->spec_b == 1 && b->spec_c == 1); /* .b class + ::before pseudo-element */
   css_stylesheet_free(ss);
}

static int has_tok(char (*toks)[CSS_CLASS_TOKEN_MAX], int n, const char *t)
{
   for (int i = 0; i < n; i++)
      if (strcmp(toks[i], t) == 0)
         return 1;
   return 0;
}

static void test_class_token_extraction(void)
{
   char toks[64][CSS_CLASS_TOKEN_MAX];
   /* className + class, string literals, dedup; dynamic {expr} is skipped */
   const char *tsx = "<div className=\"btn btn primary\">\n"
                     "  <span class='label muted'></span>\n"
                     "  <i className={dynamicOnly}></i>\n"
                     "  <b className=\"flex\"></b>\n"
                     "</div>";
   int n = css_extract_class_tokens(tsx, strlen(tsx), toks, 64);
   assert(has_tok(toks, n, "btn") && has_tok(toks, n, "primary"));
   assert(has_tok(toks, n, "label") && has_tok(toks, n, "muted") && has_tok(toks, n, "flex"));
   assert(!has_tok(toks, n, "dynamicOnly")); /* {expr} not a static class */
   /* "btn" appears twice but is de-duplicated */
   int btn = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(toks[i], "btn") == 0)
         btn++;
   assert(btn == 1);
   /* a substring like "subclassName" must not be mistaken for the attribute */
   const char *nope = "const subclassName = \"x\"; let myclass=\"y\";";
   assert(css_extract_class_tokens(nope, strlen(nope), toks, 64) == 0);
}

int main(void)
{
   test_specificity();
   test_basic_parse();
   test_at_context();
   test_malformed_resilience();
   test_custom_properties_and_important();
   test_strings_and_urls();
   test_class_token_extraction();
   printf("css_analyze: all tests passed\n");
   return 0;
}
