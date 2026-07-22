/* wfe_deliver.c -- gate.deliver re-verification policy over the verdict graph.
 * See wfe_deliver.h. Pure graph walk over the parsed def + a caller predicate;
 * no engine/DB deps, so it is unit-testable in isolation. */
#include "wfe_deliver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wfe_block_is_verdict_gate(wfe_block_type_t t)
{
   switch (t)
   {
   case WFE_BLK_GATE_ROUNDTABLE:
   case WFE_BLK_GATE_HUMAN:
   case WFE_BLK_REVIEW:
   case WFE_BLK_GATE_CI:
   case WFE_BLK_CHECK_MERGEABLE:
      return 1;
   default:
      return 0;
   }
}

static int node_index(const wfe_def_t *def, const char *id)
{
   if (!id || !id[0])
      return -1;
   for (int i = 0; i < def->n_nodes; i++)
      if (strcmp(def->nodes[i].id, id) == 0)
         return i;
   return -1;
}

/* Mark nodes reachable from `start_idx`. If success_only, follow only the
 * success edges (on_pass, next); else follow every control edge. Iterative
 * work-list so cycles (on_fail loop-backs) terminate. */
static void mark_reach(const wfe_def_t *def, int start_idx, int success_only, int *seen)
{
   int *stack = calloc((size_t)(def->n_nodes > 0 ? def->n_nodes : 1), sizeof(int));
   if (!stack)
      return;
   int sp = 0;
   if (start_idx >= 0)
      stack[sp++] = start_idx;
   while (sp > 0)
   {
      int i = stack[--sp];
      if (i < 0 || seen[i])
         continue;
      seen[i] = 1;
      const wfe_node_t *n = &def->nodes[i];
      const char *edges[3];
      int ne = 0;
      edges[ne++] = n->on_pass;
      edges[ne++] = n->next;
      if (!success_only)
         edges[ne++] = n->on_fail;
      for (int e = 0; e < ne; e++)
      {
         int j = node_index(def, edges[e]);
         if (j >= 0 && !seen[j])
            stack[sp++] = j;
      }
   }
   free(stack);
}

int wfe_deliver_reverify(const wfe_def_t *def, const char *deliver_id,
                         wfe_gate_advanced_fn advanced, void *ctx, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!def || !deliver_id || !advanced)
   {
      if (err)
         snprintf(err, errlen, "deliver reverify: bad args");
      return -1;
   }
   int deliver_idx = node_index(def, deliver_id);
   if (deliver_idx < 0)
   {
      snprintf(err, errlen, "deliver reverify: node '%s' not found", deliver_id);
      return -1;
   }

   int start_idx = node_index(def, def->start);
   int *from_start = calloc((size_t)(def->n_nodes > 0 ? def->n_nodes : 1), sizeof(int));
   if (!from_start)
      return -1;
   mark_reach(def, start_idx, 0 /* any edge */, from_start);

   int rc = 0;
   for (int i = 0; i < def->n_nodes && rc == 0; i++)
   {
      const wfe_node_t *n = &def->nodes[i];
      if (!wfe_block_is_verdict_gate(n->block))
         continue;
      if (!from_start[i])
         continue; /* an unreachable gate cannot gate this run */
      /* delivery-gating iff the deliver node is reachable from this gate's
       * SUCCESS edges (a gate only lets a run proceed toward deliver via
       * on_pass/next). */
      int *from_gate = calloc((size_t)(def->n_nodes > 0 ? def->n_nodes : 1), sizeof(int));
      if (!from_gate)
      {
         rc = -1;
         break;
      }
      /* Seed from the gate's success successors, not the gate itself, so a gate
       * whose only path to deliver is through its own on_pass still counts. Seed
       * from BOTH on_pass and next: a gate's success continuation is on_pass when
       * set, else next -- seeding both covers either convention (and over-seeding
       * is safe: it can only make more gates count as delivery-gating). */
      int seed_pass = node_index(def, n->on_pass);
      int seed_next = node_index(def, n->next);
      if (seed_pass >= 0)
         mark_reach(def, seed_pass, 1 /* success only */, from_gate);
      if (seed_next >= 0)
         mark_reach(def, seed_next, 1, from_gate);
      int gates_delivery = from_gate[deliver_idx];
      free(from_gate);
      if (!gates_delivery)
         continue;
      if (!advanced(n->id, ctx))
      {
         snprintf(err, errlen, "delivery-gating gate '%s' has no approving verdict record", n->id);
         rc = -1;
      }
   }
   free(from_start);
   return rc;
}
