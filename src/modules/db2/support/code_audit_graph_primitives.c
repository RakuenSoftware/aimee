#include "db2_code_audit_graph.h"

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
         const char *itail = ik;
         if (strncmp(ik, "import:", 7) == 0)
            itail = ik + 7;
         else if (strncmp(ik, "reference:", 10) == 0)
            itail = ik + 10;
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

#define DB2_CODE_AUDIT_MAX_NODES 4096

typedef struct
{
   char **names;
   int n;
   int cap;
} node_set_t;

static char *duplicate_name(const char *name)
{
   size_t size = strlen(name) + 1;
   char *copy = malloc(size);
   if (copy)
      memcpy(copy, name, size);
   return copy;
}

static int node_intern(node_set_t *set, const char *name)
{
   for (int i = 0; i < set->n; i++)
      if (strcmp(set->names[i], name) == 0)
         return i;
   if (set->n >= DB2_CODE_AUDIT_MAX_NODES)
      return -1;
   if (set->n >= set->cap)
   {
      int new_cap = set->cap ? set->cap * 2 : 64;
      char **names = realloc(set->names, (size_t)new_cap * sizeof(char *));
      if (!names)
         return -1;
      set->names = names;
      set->cap = new_cap;
   }
   set->names[set->n] = duplicate_name(name);
   if (!set->names[set->n])
      return -1;
   return set->n++;
}

static char *render_cycle(int *stack, int start, int stack_size, node_set_t *set)
{
   size_t cap = 64;
   for (int i = start; i < stack_size; i++)
      cap += strlen(set->names[stack[i]]) + 4;
   cap += strlen(set->names[stack[start]]) + 4;
   char *buffer = malloc(cap);
   if (!buffer)
      return NULL;
   size_t position = 0;
   for (int i = start; i < stack_size; i++)
      position +=
          (size_t)snprintf(buffer + position, cap - position, "%s -> ", set->names[stack[i]]);
   snprintf(buffer + position, cap - position, "%s", set->names[stack[start]]);
   return buffer;
}

static int already_reported(char **out, int count, const char *cycle)
{
   for (int i = 0; i < count; i++)
      if (strcmp(out[i], cycle) == 0)
         return 1;
   return 0;
}

static void find_cycles(int node, int *state, const int *edge_from, const int *edge_to,
                        int edge_count, int *stack, int *stack_size, node_set_t *set, char **out,
                        int *count, int max)
{
   if (*count >= max)
      return;
   state[node] = 1;
   stack[(*stack_size)++] = node;
   for (int edge = 0; edge < edge_count && *count < max; edge++)
   {
      if (edge_from[edge] != node)
         continue;
      int next = edge_to[edge];
      if (next < 0)
         continue;
      if (state[next] == 1)
      {
         int start = -1;
         for (int i = 0; i < *stack_size; i++)
            if (stack[i] == next)
            {
               start = i;
               break;
            }
         if (start >= 0)
         {
            char *cycle = render_cycle(stack, start, *stack_size, set);
            if (cycle)
            {
               if (!already_reported(out, *count, cycle))
                  out[(*count)++] = cycle;
               else
                  free(cycle);
            }
         }
      }
      else if (state[next] == 0)
      {
         find_cycles(next, state, edge_from, edge_to, edge_count, stack, stack_size, set, out,
                     count, max);
      }
   }
   state[node] = 2;
   (*stack_size)--;
}

int code_audit_find_cycles(const audit_edge_t *edges, int n_edges, char **out, int max)
{
   if (!edges || n_edges <= 0 || max <= 0)
      return 0;
   node_set_t set = {0};
   int *edge_from = calloc((size_t)n_edges, sizeof(int));
   int *edge_to = calloc((size_t)n_edges, sizeof(int));
   if (!edge_from || !edge_to)
   {
      free(edge_from);
      free(edge_to);
      return 0;
   }
   for (int edge = 0; edge < n_edges; edge++)
   {
      edge_from[edge] = edges[edge].from ? node_intern(&set, edges[edge].from) : -1;
      edge_to[edge] = edges[edge].to ? node_intern(&set, edges[edge].to) : -1;
   }

   int count = 0;
   if (set.n > 0)
   {
      int *state = calloc((size_t)set.n, sizeof(int));
      int *stack = calloc((size_t)set.n, sizeof(int));
      if (state && stack)
      {
         int stack_size = 0;
         for (int node = 0; node < set.n && count < max; node++)
            if (state[node] == 0)
               find_cycles(node, state, edge_from, edge_to, n_edges, stack, &stack_size, &set, out,
                           &count, max);
      }
      free(state);
      free(stack);
   }

   for (int i = 0; i < set.n; i++)
      free(set.names[i]);
   free(set.names);
   free(edge_from);
   free(edge_to);
   return count;
}
