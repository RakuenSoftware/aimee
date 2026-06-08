/* code_audit_graph.c: see code_audit_graph.h. Pure — no DB, no I/O. */
#include "code_audit_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int code_audit_dead_exports(const char *const *exports, int n_exports, const char *const *imports,
                            int n_imports, const char **out, int max)
{
   int count = 0;
   for (int i = 0; i < n_exports && count < max; i++)
   {
      const char *ek = exports[i];
      if (!ek)
         continue;
      const char *etail = strncmp(ek, "export:", 7) == 0 ? ek + 7 : ek;
      int consumed = 0;
      for (int j = 0; j < n_imports; j++)
      {
         const char *ik = imports[j];
         if (!ik)
            continue;
         const char *itail = strncmp(ik, "import:", 7) == 0 ? ik + 7 : ik;
         if (strcmp(etail, itail) == 0)
         {
            consumed = 1;
            break;
         }
      }
      if (!consumed)
         out[count++] = ek;
   }
   return count;
}

/* ---- cycle finder ---- */

#define CA_MAX_NODES 4096

typedef struct
{
   char **names;
   int n, cap;
} node_set_t;

static int node_intern(node_set_t *s, const char *name)
{
   for (int i = 0; i < s->n; i++)
      if (strcmp(s->names[i], name) == 0)
         return i;
   if (s->n >= CA_MAX_NODES)
      return -1;
   if (s->n >= s->cap)
   {
      int nc = s->cap ? s->cap * 2 : 64;
      char **np = realloc(s->names, (size_t)nc * sizeof(char *));
      if (!np)
         return -1;
      s->names = np;
      s->cap = nc;
   }
   s->names[s->n] = strdup(name);
   if (!s->names[s->n])
      return -1;
   return s->n++;
}

/* Render the cycle stack[start..sp-1] then back to stack[start] as
 * "a -> b -> … -> a". Returns malloc'd string or NULL. */
static char *render_cycle(int *stack, int start, int sp, node_set_t *ns)
{
   size_t cap = 64;
   for (int k = start; k < sp; k++)
      cap += strlen(ns->names[stack[k]]) + 4;
   cap += strlen(ns->names[stack[start]]) + 4;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t pos = 0;
   for (int k = start; k < sp; k++)
      pos += (size_t)snprintf(buf + pos, cap - pos, "%s -> ", ns->names[stack[k]]);
   snprintf(buf + pos, cap - pos, "%s", ns->names[stack[start]]); /* close the loop */
   return buf;
}

static int already_reported(char **out, int count, const char *s)
{
   for (int i = 0; i < count; i++)
      if (strcmp(out[i], s) == 0)
         return 1;
   return 0;
}

static void dfs(int u, int *state, const int *efrom, const int *eto, int n_edges, int *stack,
                int *sp, node_set_t *ns, char **out, int *count, int max)
{
   if (*count >= max)
      return;
   state[u] = 1; /* gray: on stack */
   stack[(*sp)++] = u;
   for (int e = 0; e < n_edges && *count < max; e++)
   {
      if (efrom[e] != u)
         continue;
      int v = eto[e];
      if (v < 0)
         continue;
      if (state[v] == 1)
      {
         int start = -1;
         for (int k = 0; k < *sp; k++)
            if (stack[k] == v)
            {
               start = k;
               break;
            }
         if (start >= 0)
         {
            char *c = render_cycle(stack, start, *sp, ns);
            if (c)
            {
               if (!already_reported(out, *count, c))
                  out[(*count)++] = c;
               else
                  free(c);
            }
         }
      }
      else if (state[v] == 0)
      {
         dfs(v, state, efrom, eto, n_edges, stack, sp, ns, out, count, max);
      }
   }
   state[u] = 2; /* black */
   (*sp)--;
}

int code_audit_find_cycles(const audit_edge_t *edges, int n_edges, char **out, int max)
{
   if (!edges || n_edges <= 0 || max <= 0)
      return 0;
   node_set_t ns = {0};
   int *efrom = calloc((size_t)n_edges, sizeof(int));
   int *eto = calloc((size_t)n_edges, sizeof(int));
   if (!efrom || !eto)
   {
      free(efrom);
      free(eto);
      return 0;
   }
   for (int e = 0; e < n_edges; e++)
   {
      efrom[e] = (edges[e].from) ? node_intern(&ns, edges[e].from) : -1;
      eto[e] = (edges[e].to) ? node_intern(&ns, edges[e].to) : -1;
   }

   int count = 0;
   if (ns.n > 0)
   {
      int *state = calloc((size_t)ns.n, sizeof(int));
      int *stack = calloc((size_t)ns.n, sizeof(int));
      if (state && stack)
      {
         int sp = 0;
         for (int u = 0; u < ns.n && count < max; u++)
            if (state[u] == 0)
               dfs(u, state, efrom, eto, n_edges, stack, &sp, &ns, out, &count, max);
      }
      free(state);
      free(stack);
   }

   for (int i = 0; i < ns.n; i++)
      free(ns.names[i]);
   free(ns.names);
   free(efrom);
   free(eto);
   return count;
}
