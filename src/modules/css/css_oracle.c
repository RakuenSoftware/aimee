/* css_oracle.c: interim static declaration-set equivalence oracle. See css_oracle.h. */
#include "css_oracle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CSS_ORACLE_BANNER =
    "STATIC declaration-set equivalence only: this oracle compares the resolved "
    "property:value sets and CANNOT see cascade/layout interactions that require "
    "rendering, nor does it expand shorthands. Differences it cannot prove equal "
    "are reported as changes (conservative). Use the full computed-style oracle "
    "for final sign-off.";

/* A resolved property entry after cascade collapse. */
typedef struct
{
   char property[CSS_PROPERTY_MAX];
   char value[CSS_VALUE_MAX];
   int important;
} css_resolved_t;

/* Lowercase + trim a property name; collapse internal whitespace in a value.
 * CSS property names are case-insensitive; values are kept verbatim except for
 * whitespace normalization (so "0  auto" == "0 auto"). */
static void css_norm_property(const char *in, char *out, size_t cap)
{
   while (*in && isspace((unsigned char)*in))
      in++;
   size_t j = 0;
   for (; *in && j < cap - 1; in++)
      out[j++] = (char)tolower((unsigned char)*in);
   while (j > 0 && isspace((unsigned char)out[j - 1]))
      j--;
   out[j] = '\0';
}

static void css_norm_value(const char *in, char *out, size_t cap)
{
   while (*in && isspace((unsigned char)*in))
      in++;
   size_t j = 0;
   int prev_space = 0;
   for (; *in && j < cap - 1; in++)
   {
      if (isspace((unsigned char)*in))
      {
         if (!prev_space && j > 0)
            out[j++] = ' ';
         prev_space = 1;
      }
      else
      {
         out[j++] = *in;
         prev_space = 0;
      }
   }
   while (j > 0 && out[j - 1] == ' ')
      j--;
   out[j] = '\0';
}

static css_resolved_t *resolve_find(css_resolved_t *set, int n, const char *prop)
{
   for (int i = 0; i < n; i++)
      if (strcmp(set[i].property, prop) == 0)
         return &set[i];
   return NULL;
}

/* Collapse a declaration list to the cascade-resolved property set. Within one
 * unit, the winner per property is: !important beats non-important; among equal
 * importance, later source order wins. Returns the count; set must hold >= n. */
static int css_oracle_resolve(const css_declaration_t *decls, int n, css_resolved_t *set)
{
   int count = 0;
   for (int i = 0; i < n && decls; i++)
   {
      char prop[CSS_PROPERTY_MAX];
      css_norm_property(decls[i].property, prop, sizeof(prop));
      if (!prop[0])
         continue;
      css_resolved_t *e = resolve_find(set, count, prop);
      int imp = decls[i].important ? 1 : 0;
      if (!e)
      {
         e = &set[count++];
         snprintf(e->property, sizeof(e->property), "%s", prop);
         css_norm_value(decls[i].value, e->value, sizeof(e->value));
         e->important = imp;
      }
      else if (imp || !e->important)
      {
         /* new important always wins; equal importance -> later wins; a
          * non-important never overrides an existing important. */
         css_norm_value(decls[i].value, e->value, sizeof(e->value));
         e->important = imp;
      }
   }
   return count;
}

static void oracle_add_diff(css_oracle_result_t *r, int *cap, const char *prop, const char *bval,
                            int bimp, const char *aval, int aimp, css_oracle_diff_kind_t kind)
{
   if (r->diff_count >= *cap)
   {
      int nc = *cap == 0 ? 8 : *cap * 2;
      css_oracle_diff_t *grown = realloc(r->diffs, (size_t)nc * sizeof(css_oracle_diff_t));
      if (!grown)
         return;
      r->diffs = grown;
      *cap = nc;
   }
   css_oracle_diff_t *d = &r->diffs[r->diff_count++];
   memset(d, 0, sizeof(*d));
   snprintf(d->property, sizeof(d->property), "%s", prop);
   snprintf(d->before_value, sizeof(d->before_value), "%s", bval ? bval : "");
   snprintf(d->after_value, sizeof(d->after_value), "%s", aval ? aval : "");
   d->before_important = bimp;
   d->after_important = aimp;
   d->kind = kind;
}

css_oracle_result_t *css_oracle_compare(const css_declaration_t *before, int nbefore,
                                        const css_declaration_t *after, int nafter)
{
   css_oracle_result_t *r = calloc(1, sizeof(*r));
   if (!r)
      return NULL;
   r->limitation = CSS_ORACLE_BANNER;
   r->equivalent = 1;

   css_resolved_t *bset = NULL, *aset = NULL;
   int nb = 0, na = 0;
   if (nbefore > 0)
   {
      bset = calloc((size_t)nbefore, sizeof(css_resolved_t));
      if (!bset)
      {
         free(r);
         return NULL;
      }
      nb = css_oracle_resolve(before, nbefore, bset);
   }
   if (nafter > 0)
   {
      aset = calloc((size_t)nafter, sizeof(css_resolved_t));
      if (!aset)
      {
         free(bset);
         free(r);
         return NULL;
      }
      na = css_oracle_resolve(after, nafter, aset);
   }

   int cap = 0;
   /* changed + removed: walk BEFORE */
   for (int i = 0; i < nb; i++)
   {
      css_resolved_t *a = resolve_find(aset, na, bset[i].property);
      if (!a)
         oracle_add_diff(r, &cap, bset[i].property, bset[i].value, bset[i].important, "", 0,
                         CSS_ORACLE_REMOVED);
      else if (strcmp(bset[i].value, a->value) != 0 || bset[i].important != a->important)
         oracle_add_diff(r, &cap, bset[i].property, bset[i].value, bset[i].important, a->value,
                         a->important, CSS_ORACLE_CHANGED);
   }
   /* added: properties in AFTER not in BEFORE */
   for (int i = 0; i < na; i++)
   {
      if (!resolve_find(bset, nb, aset[i].property))
         oracle_add_diff(r, &cap, aset[i].property, "", 0, aset[i].value, aset[i].important,
                         CSS_ORACLE_ADDED);
   }

   r->equivalent = (r->diff_count == 0);
   free(bset);
   free(aset);
   return r;
}

void css_oracle_result_free(css_oracle_result_t *r)
{
   if (!r)
      return;
   free(r->diffs);
   free(r);
}
