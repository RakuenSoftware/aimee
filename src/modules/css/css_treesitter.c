/* css_treesitter.c: tree-sitter front-end for the CSS structural analyzer.
 * See css_treesitter.h.
 *
 * Real implementation under -DAIMEE_TREESITTER (links the fetched tree-sitter
 * runtime + the vendored tree-sitter-css grammar); otherwise a stub returning
 * NULL so css_analyze falls back to its hand-rolled parser, exactly as
 * extract_calls falls back when code_treesitter_calls reports unavailable.
 *
 * The walk produces the same css_stylesheet_t the hand-rolled parser does, so
 * the two are interchangeable and the fallback is a fallback rather than a
 * different answer. Where they differ, the grammar is the more precise of the
 * two: specificity is counted from selector node types rather than from
 * characters, so `a:is(.x, #y)` and `[data-x="}"]` are weighed rather than
 * marked uncertain, and a construct genuinely outside the specification's
 * counting rules is still marked rather than guessed at.
 */

#include "css_treesitter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIMEE_TREESITTER

#include "tree_sitter/api.h"

/* Grammar entry point, defined in the vendored parser.c. Declared here rather
 * than shared with code_treesitter.c so neither file has to include the
 * other's types; both are declaring the same symbol from the same object. */
const TSLanguage *tree_sitter_css(void);
/* The JSX side of the same join: a component's markup names the classes a
 * stylesheet defines, and tsx is the grammar that reads it. */
const TSLanguage *tree_sitter_tsx(void);

/* The same hard caps the hand-rolled parser applies, so a pathological input
 * is bounded identically whichever path reads it. */
#define CSS_TS_MAX_RULES          200000
#define CSS_TS_MAX_DECLS_PER_RULE 8192
#define CSS_TS_MAX_SELECTORS_LIST 512

typedef struct
{
   const char *text;
   size_t len;
   css_stylesheet_t *sheet;
} css_ts_ctx_t;

/* Copy a node's source span into buf, truncating rather than overflowing --
 * the analyzer's contract is that over-long tokens truncate. */
static void css_ts_text(const css_ts_ctx_t *ctx, TSNode node, char *buf, size_t cap)
{
   if (cap == 0)
      return;
   buf[0] = '\0';
   if (ts_node_is_null(node))
      return;
   uint32_t start = ts_node_start_byte(node);
   uint32_t end = ts_node_end_byte(node);
   if (start >= ctx->len || end <= start)
      return;
   if (end > ctx->len)
      end = (uint32_t)ctx->len;
   size_t n = end - start;
   if (n > cap - 1)
      n = cap - 1;
   memcpy(buf, ctx->text + start, n);
   buf[n] = '\0';
}

static css_rule_t *css_ts_add_rule(css_stylesheet_t *sheet)
{
   if (sheet->rule_count >= CSS_TS_MAX_RULES)
   {
      sheet->truncated = 1;
      return NULL;
   }
   /* Grow geometrically, as css_ss_add_rule does, so a large generated
    * stylesheet stays amortized O(n). */
   if ((sheet->rule_count & (sheet->rule_count - 1)) == 0)
   {
      int newcap = sheet->rule_count == 0 ? 1 : sheet->rule_count * 2;
      css_rule_t *grown = realloc(sheet->rules, (size_t)newcap * sizeof(css_rule_t));
      if (!grown)
         return NULL;
      sheet->rules = grown;
   }
   css_rule_t *rule = &sheet->rules[sheet->rule_count];
   memset(rule, 0, sizeof(*rule));
   sheet->rule_count++;
   return rule;
}

/* --- specificity, counted from the tree ---------------------------------- */

static void css_ts_spec(const css_ts_ctx_t *ctx, TSNode node, int *a, int *b, int *c,
                        int *uncertain);

/* The first `arguments` child, or a null node. */
static TSNode css_ts_arguments(TSNode node)
{
   uint32_t children = ts_node_child_count(node);
   for (uint32_t i = 0; i < children; i++)
   {
      TSNode child = ts_node_child(node, i);
      if (strcmp(ts_node_type(child), "arguments") == 0)
         return child;
   }
   return ts_node_child(node, ts_node_child_count(node)); /* null */
}

/* Does an argument list hold selectors, as `:is(.a)` and
 * `:nth-child(odd of .visible)` do, rather than the values `:nth-child(2n+1)`
 * and `:lang(en)` hold? */
static int css_ts_arguments_hold_selectors(TSNode arguments)
{
   uint32_t children = ts_node_child_count(arguments);
   for (uint32_t i = 0; i < children; i++)
   {
      const char *type = ts_node_type(ts_node_child(arguments, i));
      if (strstr(type, "selector") != NULL || strcmp(type, "tag_name") == 0 ||
          strcmp(type, "class_name") == 0 || strcmp(type, "id_name") == 0)
         return 1;
   }
   return 0;
}

/* A functional pseudo-class. :where() contributes nothing; :is(), :not() and
 * :has() contribute their most specific argument; anything else counts as the
 * one pseudo-class it is. `:nth-child(... of S)` is the case the specification
 * makes context-dependent, and it is marked rather than weighed. */
static void css_ts_spec_pseudo(const css_ts_ctx_t *ctx, TSNode node, TSNode arguments, int *a,
                               int *b, int *c, int *uncertain)
{
   char name[64] = "";
   uint32_t children = ts_node_child_count(node);
   for (uint32_t i = 0; i < children; i++)
   {
      TSNode child = ts_node_child(node, i);
      if (strcmp(ts_node_type(child), "class_name") == 0)
      {
         css_ts_text(ctx, child, name, sizeof(name));
         break;
      }
   }

   if (strcmp(name, "where") == 0)
      return;

   if (strcmp(name, "is") == 0 || strcmp(name, "not") == 0 || strcmp(name, "has") == 0 ||
       strcmp(name, "matches") == 0)
   {
      /* The most specific argument, compared as the specification compares
       * specificities: a, then b, then c. */
      int best_a = 0, best_b = 0, best_c = 0;
      uint32_t argument_count = ts_node_child_count(arguments);
      for (uint32_t i = 0; i < argument_count; i++)
      {
         TSNode argument = ts_node_child(arguments, i);
         if (!ts_node_is_named(argument))
            continue;
         int ta = 0, tb = 0, tc = 0;
         css_ts_spec(ctx, argument, &ta, &tb, &tc, uncertain);
         if (ta > best_a || (ta == best_a && (tb > best_b || (tb == best_b && tc > best_c))))
         {
            best_a = ta;
            best_b = tb;
            best_c = tc;
         }
      }
      *a += best_a;
      *b += best_b;
      *c += best_c;
      return;
   }

   /* Everything else is one pseudo-class. An "of S" clause makes the match
    * depend on a selector this does not weigh, so say so. */
   if (css_ts_arguments_hold_selectors(arguments))
      *uncertain = 1;
   (*b)++;
}

/* A selector's (a, b, c) per the CSS specification, counted from node types
 * rather than from characters. Compound and combinator selectors nest -- `a.x`
 * is a class_selector holding a tag_name -- so every node is descended into,
 * and each contributes only its own weight. */
static void css_ts_spec(const css_ts_ctx_t *ctx, TSNode node, int *a, int *b, int *c,
                        int *uncertain)
{
   if (ts_node_is_null(node))
      return;
   const char *type = ts_node_type(node);
   /* A child this node has already accounted for, and must not be
    * descended into a second time. */
   TSNode counted = ts_node_child(node, ts_node_child_count(node)); /* null */

   if (strcmp(type, "id_selector") == 0)
      (*a)++;
   else if (strcmp(type, "class_selector") == 0 || strcmp(type, "attribute_selector") == 0)
      (*b)++;
   else if (strcmp(type, "pseudo_class_selector") == 0)
   {
      TSNode arguments = css_ts_arguments(node);
      if (ts_node_is_null(arguments))
         (*b)++; /* :hover */
      else
      {
         css_ts_spec_pseudo(ctx, node, arguments, a, b, c, uncertain);
         counted = arguments; /* counted above; do not descend twice */
      }
   }
   else if (strcmp(type, "pseudo_element_selector") == 0)
   {
      /* The grammar spells the element's own name as a tag_name after the
       * '::', so counting the node and then descending would weigh
       * `div::before` as three rather than two. The prefix, `div` here, sits
       * before the '::' and is still descended into. */
      (*c)++;
      uint32_t parts = ts_node_child_count(node);
      int seen_colons = 0;
      for (uint32_t i = 0; i < parts; i++)
      {
         TSNode part = ts_node_child(node, i);
         if (strcmp(ts_node_type(part), "::") == 0)
            seen_colons = 1;
         else if (seen_colons && strcmp(ts_node_type(part), "tag_name") == 0)
         {
            counted = part;
            break;
         }
      }
   }
   else if (strcmp(type, "tag_name") == 0)
      (*c)++;
   else if (strcmp(type, "nesting_selector") == 0)
      *uncertain = 1; /* context-dependent by definition */

   /* universal_selector contributes nothing, by the specification. */

   uint32_t children = ts_node_child_count(node);
   for (uint32_t i = 0; i < children; i++)
   {
      TSNode child = ts_node_child(node, i);
      if (!ts_node_is_null(counted) && ts_node_eq(child, counted))
         continue;
      css_ts_spec(ctx, child, a, b, c, uncertain);
   }
}

/* --- declarations --------------------------------------------------------- */

static void css_ts_read_declarations(const css_ts_ctx_t *ctx, TSNode block, css_declaration_t *out,
                                     int *count)
{
   *count = 0;
   uint32_t children = ts_node_child_count(block);
   for (uint32_t i = 0; i < children && *count < CSS_TS_MAX_DECLS_PER_RULE; i++)
   {
      TSNode child = ts_node_child(block, i);
      if (strcmp(ts_node_type(child), "declaration") != 0)
         continue;

      css_declaration_t *decl = &out[*count];
      memset(decl, 0, sizeof(*decl));

      /* property_name, then everything up to the terminator as the value --
       * the grammar splits a value into typed parts (color_value, call_
       * expression, plain_value), and the analyzer's contract is the value as
       * written. */
      uint32_t parts = ts_node_child_count(child);
      uint32_t value_start = 0;
      uint32_t value_end = 0;
      for (uint32_t j = 0; j < parts; j++)
      {
         TSNode part = ts_node_child(child, j);
         const char *ptype = ts_node_type(part);
         if (strcmp(ptype, "property_name") == 0)
         {
            css_ts_text(ctx, part, decl->property, sizeof(decl->property));
            continue;
         }
         if (strcmp(ptype, "important") == 0)
         {
            decl->important = 1;
            continue;
         }
         if (strcmp(ptype, ":") == 0 || strcmp(ptype, ";") == 0 || strcmp(ptype, ",") == 0)
            continue;
         if (decl->property[0] == '\0')
            continue; /* nothing before the property name belongs to the value */
         if (value_start == 0)
            value_start = ts_node_start_byte(part);
         value_end = ts_node_end_byte(part);
      }

      if (decl->property[0] == '\0')
         continue;
      if (value_end > value_start && value_start < ctx->len)
      {
         size_t n = value_end - value_start;
         if (value_start + n > ctx->len)
            n = ctx->len - value_start;
         if (n > sizeof(decl->value) - 1)
            n = sizeof(decl->value) - 1;
         memcpy(decl->value, ctx->text + value_start, n);
         decl->value[n] = '\0';
      }
      (*count)++;
   }
}

/* --- rules ---------------------------------------------------------------- */

/* One rule per selector in a comma list, each carrying a copy of the block's
 * declarations -- the same shape css_emit_rules produces. */
static void css_ts_emit_rule_set(css_ts_ctx_t *ctx, TSNode rule_set, const char *at_context)
{
   TSNode selectors = ts_node_child(rule_set, 0);
   TSNode block = ts_node_child(rule_set, ts_node_child_count(rule_set) - 1);
   if (ts_node_is_null(selectors) || ts_node_is_null(block))
      return;
   if (strcmp(ts_node_type(block), "block") != 0)
      return;

   css_declaration_t *decls = calloc(CSS_TS_MAX_DECLS_PER_RULE, sizeof(*decls));
   if (!decls)
   {
      ctx->sheet->truncated = 1;
      return;
   }
   int decl_count = 0;
   css_ts_read_declarations(ctx, block, decls, &decl_count);

   int emitted = 0;
   uint32_t children = ts_node_child_count(selectors);
   for (uint32_t i = 0; i < children && emitted < CSS_TS_MAX_SELECTORS_LIST; i++)
   {
      TSNode selector = ts_node_child(selectors, i);
      if (!ts_node_is_named(selector))
         continue; /* the commas */

      css_rule_t *rule = css_ts_add_rule(ctx->sheet);
      if (!rule)
         break;
      css_ts_text(ctx, selector, rule->selector, sizeof(rule->selector));
      rule->line = (int)ts_node_start_point(selector).row + 1;
      snprintf(rule->at_context, sizeof(rule->at_context), "%s", at_context ? at_context : "");
      css_ts_spec(ctx, selector, &rule->spec_a, &rule->spec_b, &rule->spec_c,
                  &rule->specificity_uncertain);

      if (decl_count > 0)
      {
         rule->decls = malloc((size_t)decl_count * sizeof(css_declaration_t));
         if (rule->decls)
         {
            memcpy(rule->decls, decls, (size_t)decl_count * sizeof(css_declaration_t));
            rule->decl_count = decl_count;
         }
      }
      emitted++;
   }
   free(decls);
}

/* An at-rule that holds a declaration block of its own rather than nested
 * rules -- @font-face, @page. The hand-rolled parser gives these a synthetic
 * selector and marks them uncertain, because the prelude is not a selector. */
static void css_ts_emit_at_block(css_ts_ctx_t *ctx, TSNode node, const char *at_context)
{
   TSNode block = ts_node_child(node, ts_node_child_count(node) - 1);
   if (ts_node_is_null(block) || strcmp(ts_node_type(block), "block") != 0)
      return;

   css_rule_t *rule = css_ts_add_rule(ctx->sheet);
   if (!rule)
      return;
   /* The prelude is everything before the block. */
   uint32_t start = ts_node_start_byte(node);
   uint32_t end = ts_node_start_byte(block);
   size_t n = end > start ? end - start : 0;
   if (n > sizeof(rule->selector) - 1)
      n = sizeof(rule->selector) - 1;
   if (n && start < ctx->len)
   {
      memcpy(rule->selector, ctx->text + start, n);
      rule->selector[n] = '\0';
      /* trim the trailing whitespace the block's start byte leaves behind */
      while (n > 0 && (rule->selector[n - 1] == ' ' || rule->selector[n - 1] == '\t' ||
                       rule->selector[n - 1] == '\n' || rule->selector[n - 1] == '\r'))
         rule->selector[--n] = '\0';
   }
   rule->line = (int)ts_node_start_point(node).row + 1;
   snprintf(rule->at_context, sizeof(rule->at_context), "%s", at_context ? at_context : "");
   rule->specificity_uncertain = 1; /* an at-rule prelude is not a selector */

   css_declaration_t *decls = calloc(CSS_TS_MAX_DECLS_PER_RULE, sizeof(*decls));
   if (!decls)
      return;
   int decl_count = 0;
   css_ts_read_declarations(ctx, block, decls, &decl_count);
   if (decl_count > 0)
   {
      rule->decls = malloc((size_t)decl_count * sizeof(css_declaration_t));
      if (rule->decls)
      {
         memcpy(rule->decls, decls, (size_t)decl_count * sizeof(css_declaration_t));
         rule->decl_count = decl_count;
      }
   }
   free(decls);
}

/* Walk a node's children, carrying the innermost @media/@supports/@layer
 * prelude down to the rules inside it. */
static void css_ts_walk(css_ts_ctx_t *ctx, TSNode node, const char *at_context)
{
   uint32_t children = ts_node_child_count(node);
   for (uint32_t i = 0; i < children; i++)
   {
      TSNode child = ts_node_child(node, i);
      const char *type = ts_node_type(child);

      if (strcmp(type, "rule_set") == 0)
      {
         css_ts_emit_rule_set(ctx, child, at_context);
         continue;
      }
      if (strcmp(type, "media_statement") == 0 || strcmp(type, "supports_statement") == 0 ||
          strcmp(type, "scope_statement") == 0 || strcmp(type, "at_rule") == 0)
      {
         TSNode block = ts_node_child(child, ts_node_child_count(child) - 1);
         if (ts_node_is_null(block) || strcmp(ts_node_type(block), "block") != 0)
            continue; /* a statement at-rule: @import, @charset, @namespace */

         /* @font-face and @page hold declarations, not rules. Everything else
          * that holds a block holds rules, and its prelude becomes their
          * context. */
         char prelude[CSS_ATCONTEXT_MAX];
         uint32_t start = ts_node_start_byte(child);
         uint32_t end = ts_node_start_byte(block);
         size_t n = end > start ? end - start : 0;
         if (n > sizeof(prelude) - 1)
            n = sizeof(prelude) - 1;
         if (n && start < ctx->len)
            memcpy(prelude, ctx->text + start, n);
         prelude[n] = '\0';
         while (n > 0 && (prelude[n - 1] == ' ' || prelude[n - 1] == '\t' ||
                          prelude[n - 1] == '\n' || prelude[n - 1] == '\r'))
            prelude[--n] = '\0';

         int holds_declarations = 0;
         uint32_t block_children = ts_node_child_count(block);
         for (uint32_t j = 0; j < block_children; j++)
            if (strcmp(ts_node_type(ts_node_child(block, j)), "declaration") == 0)
            {
               holds_declarations = 1;
               break;
            }

         if (holds_declarations)
            css_ts_emit_at_block(ctx, child, at_context);
         else
            css_ts_walk(ctx, block, prelude);
         continue;
      }
      /* keyframes_statement: the hand-rolled parser skips its contents, and so
       * does this -- a keyframe offset is not a selector and has no
       * specificity. */
      if (strcmp(type, "keyframes_statement") == 0)
         continue;

      if (ts_node_named_child_count(child) > 0)
         css_ts_walk(ctx, child, at_context);
   }
}

css_stylesheet_t *css_treesitter_analyze(const char *text, size_t len)
{
   if (!text)
      return NULL;

   TSParser *parser = ts_parser_new();
   if (!parser)
      return NULL;
   if (!ts_parser_set_language(parser, tree_sitter_css()))
   {
      ts_parser_delete(parser);
      return NULL;
   }
   TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)len);
   if (!tree)
   {
      ts_parser_delete(parser);
      return NULL;
   }

   css_stylesheet_t *sheet = calloc(1, sizeof(*sheet));
   if (!sheet)
   {
      ts_tree_delete(tree);
      ts_parser_delete(parser);
      return NULL;
   }

   css_ts_ctx_t ctx = {text, len, sheet};
   css_ts_walk(&ctx, ts_tree_root_node(tree), "");

   ts_tree_delete(tree);
   ts_parser_delete(parser);
   return sheet;
}

/* --- component class tokens ----------------------------------------------- */

static int css_ts_token_seen(char (*out)[CSS_CLASS_TOKEN_MAX], int n, const char *token)
{
   for (int i = 0; i < n; i++)
      if (strcmp(out[i], token) == 0)
         return 1;
   return 0;
}

/* Split an attribute value on whitespace, de-duplicating, and append. */
static int css_ts_add_tokens(const char *text, size_t len, char (*out)[CSS_CLASS_TOKEN_MAX], int n,
                             int max)
{
   size_t i = 0;
   while (i < len && n < max)
   {
      while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
         i++;
      size_t start = i;
      while (i < len && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '\r')
         i++;
      size_t length = i - start;
      if (length == 0)
         continue;
      if (length > CSS_CLASS_TOKEN_MAX - 1)
         length = CSS_CLASS_TOKEN_MAX - 1;
      char token[CSS_CLASS_TOKEN_MAX];
      memcpy(token, text + start, length);
      token[length] = '\0';
      if (!css_ts_token_seen(out, n, token))
         snprintf(out[n++], CSS_CLASS_TOKEN_MAX, "%s", token);
   }
   return n;
}

/* A jsx_attribute is `name` then, optionally, a value. A class attribute whose
 * value is a string contributes its tokens; one whose value is a jsx_expression
 * is dynamic and contributes nothing, which is the documented contract and here
 * is a fact about the node rather than a guess about its characters. */
static void css_ts_class_attribute(const css_ts_ctx_t *ctx, TSNode attribute,
                                   char (*out)[CSS_CLASS_TOKEN_MAX], int *n, int max)
{
   if (ts_node_named_child_count(attribute) == 0)
      return;
   TSNode name = ts_node_named_child(attribute, 0);
   if (strcmp(ts_node_type(name), "property_identifier") != 0)
      return;
   char attribute_name[32];
   css_ts_text(ctx, name, attribute_name, sizeof(attribute_name));
   if (strcmp(attribute_name, "className") != 0 && strcmp(attribute_name, "class") != 0)
      return;

   if (ts_node_named_child_count(attribute) < 2)
      return;
   TSNode value = ts_node_named_child(attribute, 1);
   if (strcmp(ts_node_type(value), "string") != 0)
      return; /* jsx_expression: dynamic, and not statically resolvable */

   /* The fragments inside the quotes. An escape sequence is not a class token,
    * and a string of nothing but escapes contributes none. */
   uint32_t parts = ts_node_named_child_count(value);
   for (uint32_t i = 0; i < parts && *n < max; i++)
   {
      TSNode part = ts_node_named_child(value, i);
      if (strcmp(ts_node_type(part), "string_fragment") != 0)
         continue;
      uint32_t start = ts_node_start_byte(part);
      uint32_t end = ts_node_end_byte(part);
      if (end > ctx->len || end <= start)
         continue;
      *n = css_ts_add_tokens(ctx->text + start, end - start, out, *n, max);
   }
}

static void css_ts_walk_attributes(const css_ts_ctx_t *ctx, TSNode node,
                                   char (*out)[CSS_CLASS_TOKEN_MAX], int *n, int max)
{
   if (*n >= max)
      return;
   if (strcmp(ts_node_type(node), "jsx_attribute") == 0)
      css_ts_class_attribute(ctx, node, out, n, max);
   uint32_t children = ts_node_named_child_count(node);
   for (uint32_t i = 0; i < children && *n < max; i++)
      css_ts_walk_attributes(ctx, ts_node_named_child(node, i), out, n, max);
}

int css_treesitter_class_tokens(const char *text, size_t len, char (*out)[CSS_CLASS_TOKEN_MAX],
                                int max)
{
   if (!text || !out || max <= 0)
      return -1;

   TSParser *parser = ts_parser_new();
   if (!parser)
      return -1;
   if (!ts_parser_set_language(parser, tree_sitter_tsx()))
   {
      ts_parser_delete(parser);
      return -1;
   }
   TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)len);
   if (!tree)
   {
      ts_parser_delete(parser);
      return -1;
   }

   TSNode root = ts_tree_root_node(tree);
   /* Vue and Svelte templates, and anything else this grammar cannot read, come
    * back with errors. Rather than report the few tokens a broken parse happens
    * to reach, say nothing and let the scanner -- which reads them -- answer. */
   if (ts_node_has_error(root))
   {
      ts_tree_delete(tree);
      ts_parser_delete(parser);
      return -1;
   }

   css_ts_ctx_t ctx = {text, len, NULL};
   int n = 0;
   css_ts_walk_attributes(&ctx, root, out, &n, max);

   ts_tree_delete(tree);
   ts_parser_delete(parser);
   return n;
}

#else /* !AIMEE_TREESITTER */

css_stylesheet_t *css_treesitter_analyze(const char *text, size_t len)
{
   (void)text;
   (void)len;
   return NULL;
}

int css_treesitter_class_tokens(const char *text, size_t len, char (*out)[CSS_CLASS_TOKEN_MAX],
                                int max)
{
   (void)text;
   (void)len;
   (void)out;
   (void)max;
   return -1;
}

#endif
