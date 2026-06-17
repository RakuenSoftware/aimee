/* db2/css_insights.h: read-only CSS analysis signals derived from the style
 * graph (css_rules / css_declarations). These extend the WP-B/WP-C signal set
 * (dead rules, conflicts, duplicates) with insights that guide a migration to
 * clean, tokenised CSS:
 *   - !important audit          : where overrides are concentrated (a smell)
 *   - high-specificity selectors: ID-bearing rules that resist overriding
 *   - unused custom properties  : declared `--vars` never referenced via var()
 *   - design-token candidates   : literal colours/lengths repeated enough to
 *                                 deserve a custom property
 * All are pure reads (no app-code changes) and project-scopable. */
#ifndef DEC_DB2_CSS_INSIGHTS_H
#define DEC_DB2_CSS_INSIGHTS_H 1

#include "css_graph.h" /* CSS_GRAPH_PROJECT_MAX, MAX_PATH_LEN, CSS_*_MAX */

#ifdef __cplusplus
extern "C"
{
#endif

   /* One !important hotspot: a property and how many !important declarations of
    * it exist (with a sample file). */
   typedef struct
   {
      char project[CSS_GRAPH_PROJECT_MAX];
      char property[CSS_PROPERTY_MAX];
      int count;
      char sample_file[MAX_PATH_LEN];
   } css_important_t;

   /* A high-specificity rule (uses one or more id selectors — spec_a > 0). */
   typedef struct
   {
      char project[CSS_GRAPH_PROJECT_MAX];
      char file_path[MAX_PATH_LEN];
      char selector[CSS_SELECTOR_MAX];
      int spec_a; /* id count — the high-specificity driver */
      int spec_b; /* class/attr/pseudo-class count */
      int spec_c; /* element/pseudo-element count */
      int line;
   } css_high_spec_t;

   /* A declared custom property (`--name`) never referenced by any var(). */
   typedef struct
   {
      char project[CSS_GRAPH_PROJECT_MAX];
      char name[CSS_PROPERTY_MAX]; /* the --name, as declared */
      char file_path[MAX_PATH_LEN];
      int line;
   } css_unused_var_t;

   /* A literal value (colour or length) repeated often enough to be tokenised. */
   typedef struct
   {
      char value[CSS_VALUE_MAX];
      char kind[16]; /* "color" | "length" */
      int count;
   } css_token_cand_t;

   /* !important declarations grouped by property, most-frequent first. */
   int db2_css_important_audit(const char *project_filter, css_important_t *out, int max);

   /* Rules whose specificity includes an id (spec_a > 0), highest id-count first. */
   int db2_css_high_specificity(const char *project_filter, css_high_spec_t *out, int max);

   /* Declared `--vars` that no value references via var(). NULL-/prefix-safe:
    * `--brand` is NOT marked used just because `--brandColor` is referenced. */
   int db2_css_unused_custom_properties(const char *project_filter, css_unused_var_t *out, int max);

   /* Literal colours (#hex / rgb()/rgba()/hsl()/hsla()) and lengths (px/rem/em)
    * that recur >= min_count times across declarations (values already using
    * var() are skipped). Most-repeated first — the best tokenisation candidates. */
   int db2_css_token_candidates(const char *project_filter, int min_count, css_token_cand_t *out,
                                int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CSS_INSIGHTS_H */
