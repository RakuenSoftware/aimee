/* css_analyze.h: CSS-aware structural analyzer (style graph).
 *
 * Parses plain CSS text into a style graph: rules (selector + computed
 * specificity + at-rule context + source line) and their declarations
 * (property/value/!important). This is WP-A of the CSS migration assistant
 * (docs/proposals/pending/css-migration-assistant.md §1) and is a pure leaf
 * module: it depends only on libc and never touches the DB or the indexer.
 *
 * Scope: plain CSS only. SCSS/LESS are supersets whose final declarations and
 * specificity cannot be computed without compiling them — those are indexed
 * from their compiled CSS output, not their source (see WP-C). Selector
 * constructs outside the handled subset set `specificity_uncertain` on the
 * rule rather than guessing, so downstream signals can treat them
 * conservatively. The parser never crashes on malformed input; over-long
 * tokens truncate and pathological input is bounded by hard caps.
 */
#ifndef DEC_CSS_ANALYZE_H
#define DEC_CSS_ANALYZE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CSS_SELECTOR_MAX  1024
#define CSS_PROPERTY_MAX  128
#define CSS_VALUE_MAX     1024
#define CSS_ATCONTEXT_MAX 256

   typedef struct
   {
      char property[CSS_PROPERTY_MAX];
      char value[CSS_VALUE_MAX];
      int important; /* 1 if the declaration carried !important */
   } css_declaration_t;

   typedef struct
   {
      char selector[CSS_SELECTOR_MAX];    /* a single selector; comma lists are split */
      int spec_a;                         /* specificity: #id count */
      int spec_b;                         /* specificity: class / attr / pseudo-class count */
      int spec_c;                         /* specificity: type / pseudo-element count */
      int specificity_uncertain;          /* 1 = used a construct we cannot weight precisely */
      char at_context[CSS_ATCONTEXT_MAX]; /* enclosing @media/@supports/@layer prelude, or "" */
      int line;                           /* 1-based line of the rule's selector */
      css_declaration_t *decls;
      int decl_count;
   } css_rule_t;

   typedef struct
   {
      css_rule_t *rules;
      int rule_count;
      int truncated; /* 1 = input hit a hard cap and parsing stopped early */
   } css_stylesheet_t;

   /* Parse CSS source (text, len) into a style graph. Returns NULL only on
    * allocation failure or NULL input. Free with css_stylesheet_free. */
   css_stylesheet_t *css_analyze(const char *text, size_t len);
   void css_stylesheet_free(css_stylesheet_t *ss);

   /* Compute CSS specificity (a,b,c) for ONE selector (no comma lists). Sets
    * *uncertain to 1 when the selector contains constructs outside the handled
    * subset. Any out-param may be non-NULL; all are written. Exposed for tests. */
   void css_selector_specificity(const char *selector, int *a, int *b, int *c, int *uncertain);

#define CSS_CLASS_TOKEN_MAX 128

   /* Extract the set of static class tokens used by a component's markup
    * (className="a b" / class="a b" string literals in TSX/JSX/HTML). Dynamic
    * forms (className={expr}, template literals) are NOT statically resolvable
    * and are skipped — the caller treats a component with only dynamic classes
    * as unresolved (WP-D, no silent misses). Returns the number of UNIQUE tokens
    * written to out (<= max). */
   int css_extract_class_tokens(const char *text, size_t len, char (*out)[CSS_CLASS_TOKEN_MAX],
                                int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CSS_ANALYZE_H */
