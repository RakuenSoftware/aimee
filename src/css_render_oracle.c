/* css_render_oracle.c: rendered computed-style equivalence oracle (#4-full core).
 * See css_render_oracle.h. Pure leaf: libc + cJSON. */
#include "css_render_oracle.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CSS_RENDER_BANNER =
    "RENDERED computed-style equivalence for the captured DOM/viewport/state set "
    "ONLY: nodes are matched by ref, and any unmatched node or differing computed "
    "value is reported as a change (conservative). It does NOT cover viewports, "
    "pseudo-states, or interactions the render adapter did not capture. A missing "
    "or unparseable snapshot yields an UNKNOWN verdict, never 'equivalent'.";

const char *css_render_oracle_limitation_banner(void)
{
   return CSS_RENDER_BANNER;
}

/* CSS property names are case-insensitive; lowercase + trim. */
static void norm_property(const char *in, char *out, size_t cap)
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

/* Computed values are compared verbatim except for whitespace normalization, so
 * "rgb(0,  0, 0)" == "rgb(0, 0, 0)" but colors/units are otherwise untouched. */
static void norm_value(const char *in, char *out, size_t cap)
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

/* ---- snapshot parsing ---------------------------------------------------- */

css_render_snapshot_t *css_render_snapshot_parse(const char *json)
{
   if (!json)
      return NULL;
   cJSON *root = cJSON_Parse(json);
   if (!root)
      return NULL;
   cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
   if (!cJSON_IsArray(nodes))
   {
      cJSON_Delete(root);
      return NULL;
   }
   css_render_snapshot_t *s = calloc(1, sizeof(*s));
   if (!s)
   {
      cJSON_Delete(root);
      return NULL;
   }
   int nn = cJSON_GetArraySize(nodes);
   if (nn > 0)
   {
      s->nodes = calloc((size_t)nn, sizeof(css_render_node_t));
      if (!s->nodes)
      {
         free(s);
         cJSON_Delete(root);
         return NULL;
      }
   }
   cJSON *node = NULL;
   cJSON_ArrayForEach(node, nodes)
   {
      if (!cJSON_IsObject(node))
         continue;
      cJSON *ref = cJSON_GetObjectItemCaseSensitive(node, "ref");
      cJSON *computed = cJSON_GetObjectItemCaseSensitive(node, "computed");
      if (!cJSON_IsString(ref) || !cJSON_IsObject(computed))
         continue;
      css_render_node_t *n = &s->nodes[s->nnodes];
      snprintf(n->ref, sizeof(n->ref), "%s", ref->valuestring);
      int np = cJSON_GetArraySize(computed);
      if (np > 0)
      {
         n->props = calloc((size_t)np, sizeof(css_render_prop_t));
         if (!n->props)
            continue; /* drop this node's props; keep going (conservative) */
      }
      cJSON *prop = NULL;
      cJSON_ArrayForEach(prop, computed)
      {
         if (!cJSON_IsString(prop) || !prop->string)
            continue;
         css_render_prop_t *p = &n->props[n->nprops];
         norm_property(prop->string, p->property, sizeof(p->property));
         if (!p->property[0])
            continue;
         norm_value(prop->valuestring, p->value, sizeof(p->value));
         n->nprops++;
      }
      s->nnodes++;
   }
   cJSON_Delete(root);
   return s;
}

void css_render_snapshot_free(css_render_snapshot_t *s)
{
   if (!s)
      return;
   for (int i = 0; i < s->nnodes; i++)
      free(s->nodes[i].props);
   free(s->nodes);
   free(s);
}

/* ---- comparison ---------------------------------------------------------- */

static const css_render_node_t *find_node(const css_render_snapshot_t *s, const char *ref)
{
   for (int i = 0; i < s->nnodes; i++)
      if (strcmp(s->nodes[i].ref, ref) == 0)
         return &s->nodes[i];
   return NULL;
}

static const css_render_prop_t *find_prop(const css_render_node_t *n, const char *prop)
{
   for (int i = 0; i < n->nprops; i++)
      if (strcmp(n->props[i].property, prop) == 0)
         return &n->props[i];
   return NULL;
}

static void add_diff(css_render_result_t *r, int *cap, const char *ref, const char *prop,
                     const char *bval, const char *aval, css_render_diff_kind_t kind)
{
   if (r->diff_count >= *cap)
   {
      int nc = *cap == 0 ? 8 : *cap * 2;
      css_render_diff_t *grown = realloc(r->diffs, (size_t)nc * sizeof(css_render_diff_t));
      if (!grown)
         return; /* drop this diff; equivalence already false once any diff seen */
      r->diffs = grown;
      *cap = nc;
   }
   css_render_diff_t *d = &r->diffs[r->diff_count++];
   memset(d, 0, sizeof(*d));
   snprintf(d->ref, sizeof(d->ref), "%s", ref ? ref : "");
   snprintf(d->property, sizeof(d->property), "%s", prop ? prop : "");
   snprintf(d->before_value, sizeof(d->before_value), "%s", bval ? bval : "");
   snprintf(d->after_value, sizeof(d->after_value), "%s", aval ? aval : "");
   d->kind = kind;
}

css_render_result_t *css_render_oracle_compare(const css_render_snapshot_t *before,
                                               const css_render_snapshot_t *after)
{
   css_render_result_t *r = calloc(1, sizeof(*r));
   if (!r)
      return NULL;
   r->limitation = CSS_RENDER_BANNER;

   /* Conservative: a missing snapshot is an UNKNOWN verdict, never equivalent. */
   if (!before || !after)
   {
      r->available = 0;
      r->equivalent = 0;
      return r;
   }
   r->available = 1;

   int cap = 0;
   /* Walk BEFORE nodes: structural-missing, changed, and removed properties. */
   for (int i = 0; i < before->nnodes; i++)
   {
      const css_render_node_t *bn = &before->nodes[i];
      const css_render_node_t *an = find_node(after, bn->ref);
      if (!an)
      {
         add_diff(r, &cap, bn->ref, "", "", "", CSS_RENDER_DIFF_NODE_MISSING);
         continue;
      }
      for (int j = 0; j < bn->nprops; j++)
      {
         const css_render_prop_t *ap = find_prop(an, bn->props[j].property);
         if (!ap)
            add_diff(r, &cap, bn->ref, bn->props[j].property, bn->props[j].value, "",
                     CSS_RENDER_DIFF_REMOVED);
         else if (strcmp(bn->props[j].value, ap->value) != 0)
            add_diff(r, &cap, bn->ref, bn->props[j].property, bn->props[j].value, ap->value,
                     CSS_RENDER_DIFF_CHANGED);
      }
   }
   /* Walk AFTER nodes: structural-new nodes and added properties. */
   for (int i = 0; i < after->nnodes; i++)
   {
      const css_render_node_t *an = &after->nodes[i];
      const css_render_node_t *bn = find_node(before, an->ref);
      if (!bn)
      {
         add_diff(r, &cap, an->ref, "", "", "", CSS_RENDER_DIFF_NODE_NEW);
         continue;
      }
      for (int j = 0; j < an->nprops; j++)
      {
         if (!find_prop(bn, an->props[j].property))
            add_diff(r, &cap, an->ref, an->props[j].property, "", an->props[j].value,
                     CSS_RENDER_DIFF_ADDED);
      }
   }

   r->equivalent = (r->diff_count == 0);
   return r;
}

void css_render_result_free(css_render_result_t *r)
{
   if (!r)
      return;
   free(r->diffs);
   free(r);
}

/* ---- render adapter seam ------------------------------------------------- */

static css_render_adapter_fn g_render_adapter = NULL;

void css_render_oracle_set_adapter(css_render_adapter_fn fn)
{
   g_render_adapter = fn;
}

int css_render_oracle_has_adapter(void)
{
   return g_render_adapter != NULL;
}

css_render_status_t css_render_oracle_render(const char *html, const char *css, char **out_json,
                                             char **err)
{
   if (out_json)
      *out_json = NULL;
   if (err)
      *err = NULL;
   if (!g_render_adapter)
      return CSS_RENDER_UNAVAILABLE;
   char *local_err = NULL;
   int rc = g_render_adapter(html ? html : "", css ? css : "", out_json, &local_err);
   if (rc != 0)
   {
      if (err)
         *err = local_err;
      else
         free(local_err);
      if (out_json && *out_json)
      {
         free(*out_json);
         *out_json = NULL;
      }
      return CSS_RENDER_ERROR;
   }
   free(local_err);
   return CSS_RENDER_OK;
}
