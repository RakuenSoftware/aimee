/* css_treesitter.h: tree-sitter front-end for the CSS structural analyzer.
 *
 * The same arrangement code_treesitter.h describes for definitions and calls,
 * one output type over: built only under -DAIMEE_TREESITTER against the fetched
 * tree-sitter runtime and the vendored tree-sitter-css grammar, and reporting
 * "unavailable" otherwise so css_analyze falls back to its hand-rolled parser
 * and the default build is unchanged.
 *
 * It lives beside css_analyze rather than in code_treesitter.c because what it
 * produces is a css_stylesheet_t. code_treesitter.c exists to feed one shared
 * definition_t symbol table, and a style graph is not that; putting this there
 * would make a file every language depends on depend on one module's types.
 *
 * Why a grammar rather than the tokenizer it replaces: specificity is counted
 * from selector node types -- id_selector, class_selector, tag_name -- which is
 * what the CSS specification actually says, instead of re-scanning characters
 * and guessing at the constructs it cannot weight.
 */
#ifndef DEC_CSS_TREESITTER_H
#define DEC_CSS_TREESITTER_H 1

#include <stddef.h>

#include "css_analyze.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Parse CSS source into a style graph using the tree-sitter grammar.
    * Returns a stylesheet the caller frees with css_stylesheet_free, or NULL
    * when tree-sitter is not compiled in, the parse fails, or allocation
    * fails -- in every one of which the caller falls back to css_analyze's
    * hand-rolled parser rather than reporting an empty stylesheet. */
   css_stylesheet_t *css_treesitter_analyze(const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CSS_TREESITTER_H */
