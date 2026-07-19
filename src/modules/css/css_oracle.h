/* css_oracle.h: interim static declaration-set equivalence oracle (WP-E).
 *
 * The real correctness signal for a CSS migration is computed style / box model
 * before vs. after (the full rendered oracle, #4) — that is a deferred follow-on
 * needing a headless browser. This interim oracle is the static, render-free
 * approximation the proposal authorizes (§4): for a unit of conversion it
 * resolves the BEFORE and AFTER declaration sets (cascade collapse: later wins,
 * !important takes precedence) and diffs them property by property.
 *
 * It is STRICTLY WEAKER than the rendered oracle — it cannot see cascade/layout
 * interactions that only appear when rendered, and it does NOT expand shorthands
 * (margin vs margin-top/...). Every result therefore carries an explicit
 * limitation banner, and the oracle is CONSERVATIVE: anything it cannot prove
 * equal is reported as a difference (no silent "equivalent", proposal §8). A
 * pure leaf: depends only on libc + css_analyze.h's css_declaration_t.
 */
#ifndef DEC_CSS_ORACLE_H
#define DEC_CSS_ORACLE_H 1

#include "css_analyze.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      CSS_ORACLE_CHANGED = 1, /* property present in both, value/important differs */
      CSS_ORACLE_REMOVED = 2, /* resolved in BEFORE, absent in AFTER */
      CSS_ORACLE_ADDED = 3,   /* absent in BEFORE, resolved in AFTER */
   } css_oracle_diff_kind_t;

   typedef struct
   {
      char property[CSS_PROPERTY_MAX];
      char before_value[CSS_VALUE_MAX];
      char after_value[CSS_VALUE_MAX];
      int before_important;
      int after_important;
      css_oracle_diff_kind_t kind;
   } css_oracle_diff_t;

   typedef struct
   {
      int equivalent; /* 1 iff the resolved declaration sets are identical */
      css_oracle_diff_t *diffs;
      int diff_count;
      const char *limitation; /* static banner; not owned by the caller */
   } css_oracle_result_t;

   /* Compare two declaration sets after cascade resolution. Returns NULL only on
    * allocation failure. Free with css_oracle_result_free. */
   css_oracle_result_t *css_oracle_compare(const css_declaration_t *before, int nbefore,
                                           const css_declaration_t *after, int nafter);

   void css_oracle_result_free(css_oracle_result_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CSS_ORACLE_H */
