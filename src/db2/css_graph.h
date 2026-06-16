/* db2/css_graph.h: persistence for the CSS style graph (WP-B).
 *
 * Stores the css_analyze (WP-A) output — rules with computed specificity +
 * declarations — in DB2, keyed by the existing files(id) row a CSS file already
 * has in the code index. Mirrors db2/code_index.c: a per-file delete-then-insert
 * refresh inside one transaction, plus read helpers. The lexical class-name
 * index (file_exports) is kept untouched for backward compatibility.
 */
#ifndef DEC_DB2_CSS_GRAPH_H
#define DEC_DB2_CSS_GRAPH_H 1

#include "../headers/aimee.h" /* MAX_PATH_LEN */
#include "css_analyze.h"      /* css_rule_t */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CSS_GRAPH_PROJECT_MAX 256

   typedef struct
   {
      char project[CSS_GRAPH_PROJECT_MAX];
      char file_path[MAX_PATH_LEN];
      char selector[CSS_SELECTOR_MAX];
      int spec_a;
      int spec_b;
      int spec_c;
      int spec_uncertain;
      char at_context[CSS_ATCONTEXT_MAX];
      int line;
   } css_rule_hit_t;

   typedef struct
   {
      char project[CSS_GRAPH_PROJECT_MAX];
      char file_path[MAX_PATH_LEN];
      char selector[CSS_SELECTOR_MAX];
      char property[CSS_PROPERTY_MAX];
      char value[CSS_VALUE_MAX];
      int important;
   } css_decl_hit_t;

   /* Resolve the files(id) for (project, file_path), or -1 if absent. */
   int64_t db2_css_graph_resolve_file(const char *project, const char *file_path);

   /* Replace the entire style graph for a file (delete-then-insert in one
    * transaction). `file_id` must reference an existing files row. Returns 0 on
    * success, -1 on error. n==0 just clears the file's graph. */
   int db2_css_graph_replace(int64_t file_id, const css_rule_t *rules, int n);

   /* Convenience: resolve (project, file_path) -> file_id then replace. Returns
    * -1 if the file is not indexed yet. */
   int db2_css_graph_upsert_file(const char *project, const char *file_path,
                                 const css_rule_t *rules, int n);

   /* Find rules by exact selector across the index. Returns the number written
    * to out (<= max), or -1 on error. */
   int db2_css_graph_rules_by_selector(const char *selector, css_rule_hit_t *out, int max);

   /* Find declarations by exact property name across the index. */
   int db2_css_graph_declarations_by_property(const char *property, css_decl_hit_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CSS_GRAPH_H */
