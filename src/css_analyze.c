/* css_analyze.c: CSS structural analyzer. See css_analyze.h. */
#include "css_analyze.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hard caps so malformed/adversarial input stays bounded (never crash/over-read;
 * the existing lexical scanners use fixed buffers — match that discipline). */
#define CSS_MAX_RULES          200000
#define CSS_MAX_DECLS_PER_RULE 8192
#define CSS_MAX_SELECTORS_LIST 512

/* ---- specificity ------------------------------------------------------- */

static int css_ident_char(char c)
{
   return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '\\';
}

/* Index of the ')' that closes the '(' just before s[0..len), respecting nested
 * parens and quotes. Returns len when unbalanced. */
static size_t css_find_close_paren(const char *s, size_t len)
{
   int depth = 1;
   for (size_t i = 0; i < len; i++)
   {
      char ch = s[i];
      if (ch == '(')
         depth++;
      else if (ch == ')')
      {
         if (--depth == 0)
            return i;
      }
      else if (ch == '"' || ch == '\'')
      {
         char q = ch;
         i++;
         while (i < len && s[i] != q)
         {
            if (s[i] == '\\')
               i++;
            i++;
         }
      }
   }
   return len;
}

static void css_spec_scan(const char *s, size_t len, int *a, int *b, int *c, int *uncertain);

/* Specificity of a selector list (comma-separated) = the most specific single
 * selector in the list (lexicographic (a,b,c)). Used for :is()/:not()/:has(). */
static void css_spec_list_max(const char *s, size_t len, int *a, int *b, int *c, int *uncertain)
{
   int best_a = 0, best_b = 0, best_c = 0, found = 0;
   size_t p = 0;
   while (p <= len)
   {
      size_t q = p;
      int depth = 0;
      while (q < len)
      {
         char ch = s[q];
         if (ch == '(')
            depth++;
         else if (ch == ')')
            depth--;
         else if (ch == ',' && depth == 0)
            break;
         q++;
      }
      int ca = 0, cb = 0, cc = 0;
      css_spec_scan(s + p, q - p, &ca, &cb, &cc, uncertain);
      if (!found || ca > best_a || (ca == best_a && cb > best_b) ||
          (ca == best_a && cb == best_b && cc > best_c))
      {
         best_a = ca;
         best_b = cb;
         best_c = cc;
         found = 1;
      }
      if (q >= len)
         break;
      p = q + 1;
   }
   *a += best_a;
   *b += best_b;
   *c += best_c;
}

/* Accumulate the specificity of one complex selector into (a,b,c). */
static void css_spec_scan(const char *s, size_t len, int *a, int *b, int *c, int *uncertain)
{
   size_t i = 0;
   while (i < len)
   {
      char ch = s[i];

      if (isspace((unsigned char)ch) || ch == '>' || ch == '+' || ch == '~' || ch == ',')
      {
         i++;
         continue;
      }
      if (ch == '*')
      { /* universal selector contributes nothing */
         i++;
         continue;
      }
      if (ch == '&')
      { /* CSS nesting selector: specificity is context-dependent */
         *uncertain = 1;
         i++;
         continue;
      }
      if (ch == '#')
      {
         i++;
         while (i < len && css_ident_char(s[i]))
            i++;
         (*a)++;
         continue;
      }
      if (ch == '.')
      {
         i++;
         while (i < len && css_ident_char(s[i]))
            i++;
         (*b)++;
         continue;
      }
      if (ch == '[')
      { /* attribute selector */
         i++;
         while (i < len && s[i] != ']')
         {
            if (s[i] == '"' || s[i] == '\'')
            {
               char q = s[i];
               i++;
               while (i < len && s[i] != q)
               {
                  if (s[i] == '\\')
                     i++;
                  i++;
               }
            }
            i++;
         }
         if (i < len)
            i++; /* consume ']' */
         (*b)++;
         continue;
      }
      if (ch == ':')
      {
         i++;
         int pseudo_element = 0;
         if (i < len && s[i] == ':')
         {
            pseudo_element = 1;
            i++;
         }
         size_t name_start = i;
         while (i < len && css_ident_char(s[i]))
            i++;
         char name[64];
         size_t nl = i - name_start;
         if (nl > sizeof(name) - 1)
            nl = sizeof(name) - 1;
         for (size_t k = 0; k < nl; k++)
            name[k] = (char)tolower((unsigned char)s[name_start + k]);
         name[nl] = '\0';

         if (i < len && s[i] == '(')
         { /* functional pseudo */
            i++;
            size_t close = css_find_close_paren(s + i, len - i);
            const char *args = s + i;
            size_t arg_len = close;
            if (pseudo_element)
            {
               (*c)++; /* functional pseudo-element (rare) */
            }
            else if (strcmp(name, "where") == 0)
            {
               /* :where() contributes 0 */
            }
            else if (strcmp(name, "is") == 0 || strcmp(name, "not") == 0 ||
                     strcmp(name, "has") == 0 || strcmp(name, "matches") == 0)
            {
               css_spec_list_max(args, arg_len, a, b, c, uncertain);
            }
            else if (strncmp(name, "nth", 3) == 0)
            {
               (*b)++; /* the pseudo-class itself */
               /* :nth-child(An+B of S) — the S selector list adds specificity */
               for (size_t k = 0; k + 2 < arg_len; k++)
               {
                  if ((args[k] == 'o' || args[k] == 'O') &&
                      (args[k + 1] == 'f' || args[k + 1] == 'F') &&
                      isspace((unsigned char)args[k + 2]))
                  {
                     *uncertain = 1;
                     break;
                  }
               }
            }
            else
            {
               (*b)++; /* :lang(), :dir(), other functional pseudo-classes */
            }
            i += close;
            if (i < len && s[i] == ')')
               i++;
            continue;
         }
         /* simple pseudo */
         if (pseudo_element)
            (*c)++;
         else
            (*b)++;
         continue;
      }
      if (isalpha((unsigned char)ch) || ch == '-' || ch == '_' || ch == '\\' || ch == '|')
      { /* type selector (optionally namespaced ns|el) */
         while (i < len && (css_ident_char(s[i]) || s[i] == '|'))
            i++;
         (*c)++;
         continue;
      }
      /* anything else (stray punctuation, '%', '@', ...) is unexpected here */
      *uncertain = 1;
      i++;
   }
}

void css_selector_specificity(const char *selector, int *a, int *b, int *c, int *uncertain)
{
   int la = 0, lb = 0, lc = 0, lu = 0;
   if (selector)
      css_spec_scan(selector, strlen(selector), &la, &lb, &lc, &lu);
   if (a)
      *a = la;
   if (b)
      *b = lb;
   if (c)
      *c = lc;
   if (uncertain)
      *uncertain = lu;
}

/* ---- parser ------------------------------------------------------------ */

typedef struct
{
   const char *s;
   size_t len;
   size_t i;
   int line;
} css_parser_t;

/* Append c to buf (cap n) at *pos, truncating silently past the cap. */
static void css_buf_putc(char *buf, size_t n, size_t *pos, char c)
{
   if (*pos < n - 1)
      buf[(*pos)++] = c;
}

/* Advance one char, tracking line numbers. */
static void css_advance(css_parser_t *p)
{
   if (p->i >= p->len)
      return;
   if (p->s[p->i] == '\n')
      p->line++;
   p->i++;
}

/* Skip a C-style CSS comment if positioned at one; returns 1 if it skipped. */
static int css_skip_comment(css_parser_t *p)
{
   if (p->i + 1 < p->len && p->s[p->i] == '/' && p->s[p->i + 1] == '*')
   {
      css_advance(p);
      css_advance(p);
      while (p->i + 1 < p->len && !(p->s[p->i] == '*' && p->s[p->i + 1] == '/'))
         css_advance(p);
      if (p->i + 1 < p->len)
      {
         css_advance(p);
         css_advance(p);
      }
      else
         p->i = p->len;
      return 1;
   }
   return 0;
}

static css_stylesheet_t *css_ss_new(void)
{
   css_stylesheet_t *ss = calloc(1, sizeof(*ss));
   return ss;
}

static css_rule_t *css_ss_add_rule(css_stylesheet_t *ss)
{
   if (ss->rule_count >= CSS_MAX_RULES)
   {
      ss->truncated = 1;
      return NULL;
   }
   /* Grow geometrically (realloc only at power-of-two counts) so indexing a
    * large generated stylesheet (e.g. Tailwind output) stays amortized O(n). */
   if ((ss->rule_count & (ss->rule_count - 1)) == 0)
   {
      int newcap = ss->rule_count == 0 ? 1 : ss->rule_count * 2;
      css_rule_t *grown = realloc(ss->rules, (size_t)newcap * sizeof(css_rule_t));
      if (!grown)
         return NULL;
      ss->rules = grown;
   }
   css_rule_t *r = &ss->rules[ss->rule_count];
   memset(r, 0, sizeof(*r));
   ss->rule_count++;
   return r;
}

/* If the parser is at a quote, append the whole string literal (quotes +
 * escapes) to buf (buf/pos may be NULL to discard) and advance past it; return
 * 1. Else return 0. Strings hold structural chars ('{' '}' ';' '(' ')') that
 * must NOT be treated as syntax (e.g. content: "}", url("a;b")). */
static int css_consume_string(css_parser_t *p, char *buf, size_t n, size_t *pos)
{
   if (p->i >= p->len)
      return 0;
   char ch = p->s[p->i];
   if (ch != '"' && ch != '\'')
      return 0;
   char q = ch;
   if (buf)
      css_buf_putc(buf, n, pos, ch);
   css_advance(p);
   while (p->i < p->len && p->s[p->i] != q)
   {
      if (p->s[p->i] == '\\')
      {
         if (buf)
            css_buf_putc(buf, n, pos, p->s[p->i]);
         css_advance(p);
         if (p->i >= p->len)
            break;
      }
      if (buf)
         css_buf_putc(buf, n, pos, p->s[p->i]);
      css_advance(p);
   }
   if (p->i < p->len)
   {
      if (buf)
         css_buf_putc(buf, n, pos, p->s[p->i]);
      css_advance(p);
   }
   return 1;
}

/* Finalize the current property/value pair into r (trims, strips !important).
 * Resets the buffers. No-op for an empty property. */
static void css_emit_decl(css_rule_t *r, char *prop, size_t *prop_pos, char *val, size_t *val_pos)
{
   prop[*prop_pos] = '\0';
   val[*val_pos] = '\0';
   while (*prop_pos > 0 && isspace((unsigned char)prop[*prop_pos - 1]))
      prop[--(*prop_pos)] = '\0';
   while (*val_pos > 0 && isspace((unsigned char)val[*val_pos - 1]))
      val[--(*val_pos)] = '\0';
   if (*prop_pos > 0 && r->decl_count < CSS_MAX_DECLS_PER_RULE)
   {
      css_declaration_t cur;
      memset(&cur, 0, sizeof(cur));
      char *bang = strstr(val, "!");
      if (bang)
      {
         char tail[32];
         size_t t = 0;
         for (const char *q = bang + 1; *q && t < sizeof(tail) - 1; q++)
            if (!isspace((unsigned char)*q))
               tail[t++] = (char)tolower((unsigned char)*q);
         tail[t] = '\0';
         if (strncmp(tail, "important", 9) == 0)
         {
            cur.important = 1;
            *bang = '\0';
            size_t vl = strlen(val);
            while (vl > 0 && isspace((unsigned char)val[vl - 1]))
               val[--vl] = '\0';
         }
      }
      snprintf(cur.property, sizeof(cur.property), "%s", prop);
      snprintf(cur.value, sizeof(cur.value), "%s", val);
      css_declaration_t *grown =
          realloc(r->decls, (size_t)(r->decl_count + 1) * sizeof(css_declaration_t));
      if (grown)
      {
         r->decls = grown;
         r->decls[r->decl_count++] = cur;
      }
   }
   *prop_pos = 0;
   *val_pos = 0;
   prop[0] = val[0] = '\0';
}

/* Read a balanced declaration block (parser at '{'); fill decls into r. If a
 * nested rule block is encountered (CSS nesting), stop capturing declarations,
 * flag uncertainty, and skip to the block end. Leaves parser just past '}'. */
static void css_parse_decl_block(css_parser_t *p, css_rule_t *r)
{
   css_advance(p); /* consume '{' */
   char prop[CSS_PROPERTY_MAX];
   char val[CSS_VALUE_MAX];
   size_t prop_pos = 0, val_pos = 0;
   int in_value = 0;
   int paren_depth = 0; /* depth inside value parens, e.g. url(), calc() */
   prop[0] = val[0] = '\0';

   while (p->i < p->len)
   {
      if (css_skip_comment(p))
         continue;
      char ch = p->s[p->i];

      /* String literals hold structural chars that are not syntax. */
      if (ch == '"' || ch == '\'')
      {
         if (in_value)
            css_consume_string(p, val, sizeof(val), &val_pos);
         else
            css_consume_string(p, prop, sizeof(prop), &prop_pos);
         continue;
      }
      if (ch == '}' && paren_depth == 0)
      {
         css_advance(p);
         break;
      }
      if (ch == '{' && paren_depth == 0)
      {
         /* CSS nesting: declarations can't be computed flatly here. */
         r->specificity_uncertain = 1;
         int depth = 1;
         css_advance(p);
         while (p->i < p->len && depth > 0)
         {
            if (css_skip_comment(p))
               continue;
            if (p->s[p->i] == '"' || p->s[p->i] == '\'')
            {
               css_consume_string(p, NULL, 0, NULL);
               continue;
            }
            if (p->s[p->i] == '{')
               depth++;
            else if (p->s[p->i] == '}')
               depth--;
            css_advance(p);
         }
         prop_pos = val_pos = 0;
         in_value = 0;
         continue;
      }
      if (ch == ':' && !in_value)
      {
         in_value = 1;
         css_advance(p);
         continue;
      }
      if (ch == '(' && in_value)
      {
         paren_depth++;
         css_buf_putc(val, sizeof(val), &val_pos, ch);
         css_advance(p);
         continue;
      }
      if (ch == ')' && in_value)
      {
         if (paren_depth > 0)
            paren_depth--;
         css_buf_putc(val, sizeof(val), &val_pos, ch);
         css_advance(p);
         continue;
      }
      if (ch == ';' && paren_depth == 0)
      {
         css_advance(p);
         css_emit_decl(r, prop, &prop_pos, val, &val_pos);
         in_value = 0;
         continue;
      }
      if (in_value)
      {
         if (!(val_pos == 0 && isspace((unsigned char)ch)))
            css_buf_putc(val, sizeof(val), &val_pos, ch);
      }
      else if (!isspace((unsigned char)ch) || prop_pos > 0)
         css_buf_putc(prop, sizeof(prop), &prop_pos, ch);
      css_advance(p);
   }

   /* trailing declaration without a closing ';' */
   if (in_value)
      css_emit_decl(r, prop, &prop_pos, val, &val_pos);
}

/* Skip a balanced { ... } block without capturing (for @keyframes etc.). */
static void css_skip_block(css_parser_t *p)
{
   if (p->i >= p->len || p->s[p->i] != '{')
      return;
   int depth = 1;
   css_advance(p);
   while (p->i < p->len && depth > 0)
   {
      if (css_skip_comment(p))
         continue;
      if (p->s[p->i] == '{')
         depth++;
      else if (p->s[p->i] == '}')
         depth--;
      css_advance(p);
   }
}

/* Split a selector list on top-level commas and emit one rule per selector,
 * each sharing a copy of `decls`. */
static void css_emit_rules(css_stylesheet_t *ss, const char *sel_list, int line,
                           const char *at_context, css_declaration_t *decls, int decl_count)
{
   size_t len = strlen(sel_list);
   size_t p = 0;
   int emitted = 0;
   while (p <= len && emitted < CSS_MAX_SELECTORS_LIST)
   {
      size_t q = p;
      int depth = 0;
      while (q < len)
      {
         char ch = sel_list[q];
         if (ch == '(' || ch == '[')
            depth++;
         else if (ch == ')' || ch == ']')
            depth--;
         else if (ch == ',' && depth == 0)
            break;
         q++;
      }
      /* trim one selector [p,q) */
      size_t a = p, bnd = q;
      while (a < bnd && isspace((unsigned char)sel_list[a]))
         a++;
      while (bnd > a && isspace((unsigned char)sel_list[bnd - 1]))
         bnd--;
      if (bnd > a)
      {
         css_rule_t *r = css_ss_add_rule(ss);
         if (!r)
            return;
         size_t sl = bnd - a;
         if (sl > CSS_SELECTOR_MAX - 1)
            sl = CSS_SELECTOR_MAX - 1;
         memcpy(r->selector, sel_list + a, sl);
         r->selector[sl] = '\0';
         r->line = line;
         snprintf(r->at_context, sizeof(r->at_context), "%s", at_context ? at_context : "");
         int ua = 0, ub = 0, uc = 0, un = 0;
         css_spec_scan(r->selector, strlen(r->selector), &ua, &ub, &uc, &un);
         r->spec_a = ua;
         r->spec_b = ub;
         r->spec_c = uc;
         r->specificity_uncertain = un;
         if (decl_count > 0)
         {
            r->decls = malloc((size_t)decl_count * sizeof(css_declaration_t));
            if (r->decls)
            {
               memcpy(r->decls, decls, (size_t)decl_count * sizeof(css_declaration_t));
               r->decl_count = decl_count;
            }
         }
         emitted++;
      }
      if (q >= len)
         break;
      p = q + 1;
   }
}

css_stylesheet_t *css_analyze(const char *text, size_t len)
{
   if (!text)
      return NULL;
   css_stylesheet_t *ss = css_ss_new();
   if (!ss)
      return NULL;

   css_parser_t p = {text, len, 0, 1};

   /* at-context stack: innermost prelude + the depth at which it was pushed */
   char atctx[CSS_ATCONTEXT_MAX] = "";
   int atctx_block_depth[64];
   char atctx_stack[64][CSS_ATCONTEXT_MAX];
   int atctx_top = 0;
   int brace_depth = 0;

   char prelude[CSS_SELECTOR_MAX];
   size_t prelude_pos = 0;
   int prelude_line = 1;

   while (p.i < p.len)
   {
      if (css_skip_comment(&p))
         continue;
      /* A quoted string in a selector (e.g. [data-x="}"]) holds structural
       * chars that are not syntax — consume it verbatim into the prelude. */
      if (p.s[p.i] == '"' || p.s[p.i] == '\'')
      {
         if (prelude_pos == 0)
            prelude_line = p.line;
         css_consume_string(&p, prelude, sizeof(prelude), &prelude_pos);
         continue;
      }
      char ch = p.s[p.i];

      if (ch == '{')
      {
         prelude[prelude_pos] = '\0';
         /* trim prelude */
         size_t a = 0, b = prelude_pos;
         while (a < b && isspace((unsigned char)prelude[a]))
            a++;
         while (b > a && isspace((unsigned char)prelude[b - 1]))
            b--;
         char trimmed[CSS_SELECTOR_MAX];
         size_t tl = b - a;
         if (tl > sizeof(trimmed) - 1)
            tl = sizeof(trimmed) - 1;
         memcpy(trimmed, prelude + a, tl);
         trimmed[tl] = '\0';

         if (trimmed[0] == '@')
         {
            /* classify the at-rule */
            char kw[32];
            size_t k = 0;
            for (size_t j = 1; j < tl && k < sizeof(kw) - 1 && css_ident_char(trimmed[j]); j++)
               kw[k++] = (char)tolower((unsigned char)trimmed[j]);
            kw[k] = '\0';
            if (strcmp(kw, "media") == 0 || strcmp(kw, "supports") == 0 ||
                strcmp(kw, "layer") == 0 || strcmp(kw, "container") == 0 ||
                strcmp(kw, "scope") == 0)
            {
               /* context block: push, recurse into inner rules */
               brace_depth++;
               if (atctx_top < 64)
               {
                  snprintf(atctx_stack[atctx_top], CSS_ATCONTEXT_MAX, "%s", trimmed);
                  atctx_block_depth[atctx_top] = brace_depth;
                  atctx_top++;
               }
               /* rebuild combined context (innermost shown) */
               snprintf(atctx, sizeof(atctx), "%s",
                        atctx_top > 0 ? atctx_stack[atctx_top - 1] : "");
               css_advance(&p); /* consume '{' */
               prelude_pos = 0;
               continue;
            }
            if (strcmp(kw, "font-face") == 0 || strcmp(kw, "page") == 0)
            {
               /* declaration block with a synthetic selector */
               css_rule_t *r = css_ss_add_rule(ss);
               if (r)
               {
                  snprintf(r->selector, sizeof(r->selector), "%s", trimmed);
                  r->line = prelude_line;
                  snprintf(r->at_context, sizeof(r->at_context), "%s", atctx);
                  r->specificity_uncertain = 1; /* at-rule, not a real selector */
                  css_parse_decl_block(&p, r);
               }
               else
                  css_skip_block(&p);
               prelude_pos = 0;
               continue;
            }
            /* @keyframes / unknown at-rule with a block: skip its contents */
            css_skip_block(&p);
            prelude_pos = 0;
            continue;
         }

         /* normal selector list → declaration block */
         {
            css_rule_t scratch;
            memset(&scratch, 0, sizeof(scratch));
            css_parse_decl_block(&p, &scratch);
            css_emit_rules(ss, trimmed, prelude_line, atctx, scratch.decls, scratch.decl_count);
            /* propagate nesting-uncertainty to the emitted rules */
            if (scratch.specificity_uncertain)
               for (int ri = ss->rule_count - 1; ri >= 0 && ss->rules[ri].line == prelude_line;
                    ri--)
                  ss->rules[ri].specificity_uncertain = 1;
            free(scratch.decls);
         }
         prelude_pos = 0;
         continue;
      }

      if (ch == '}')
      {
         /* pop any at-context opened at this depth */
         if (atctx_top > 0 && atctx_block_depth[atctx_top - 1] == brace_depth)
            atctx_top--;
         if (brace_depth > 0)
            brace_depth--;
         snprintf(atctx, sizeof(atctx), "%s", atctx_top > 0 ? atctx_stack[atctx_top - 1] : "");
         css_advance(&p);
         prelude_pos = 0;
         continue;
      }

      if (ch == ';')
      {
         /* a statement at-rule (@import/@charset/@namespace) or stray ';' */
         css_advance(&p);
         prelude_pos = 0;
         continue;
      }

      if (prelude_pos == 0 && !isspace((unsigned char)ch))
         prelude_line = p.line;
      if (!(prelude_pos == 0 && isspace((unsigned char)ch)))
         css_buf_putc(prelude, sizeof(prelude), &prelude_pos, ch);
      css_advance(&p);
   }

   return ss;
}

void css_stylesheet_free(css_stylesheet_t *ss)
{
   if (!ss)
      return;
   for (int i = 0; i < ss->rule_count; i++)
      free(ss->rules[i].decls);
   free(ss->rules);
   free(ss);
}
