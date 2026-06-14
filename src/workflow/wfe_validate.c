/* wfe_validate.c -- workflow graph validator: typed-handle resolution + graph
 * health + the single-lens floor. Returns 0 on success; nonzero + err on the
 * first violation found. */
#include "wfe_def.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int param_bool(const cJSON *params, const char *key, int dflt)
{
   if (!params)
      return dflt;
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(params, key);
   if (!it)
      return dflt;
   if (cJSON_IsBool(it))
      return cJSON_IsTrue(it) ? 1 : 0;
   return dflt;
}

static const char *param_str(const cJSON *params, const char *key)
{
   if (!params)
      return NULL;
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(params, key);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* validate one node's input bindings against the type system. */
static int check_inputs(const wfe_def_t *def, const wfe_node_t *n, char *err, size_t errlen)
{
   /* duplicate input slot = "two producers feeding one input without select" */
   for (int i = 0; i < n->n_ins; i++)
      for (int j = i + 1; j < n->n_ins; j++)
         if (strcmp(n->ins[i].input_name, n->ins[j].input_name) == 0)
         {
            snprintf(err, errlen, "node '%s': input '%s' bound twice (ambiguous producer)", n->id,
                     n->ins[i].input_name);
            return -1;
         }

   if (wfe_block_requires_input(n->block) && n->n_ins == 0)
   {
      snprintf(err, errlen, "node '%s' (%s) requires an input binding", n->id,
               wfe_block_name(n->block));
      return -1;
   }

   for (int i = 0; i < n->n_ins; i++)
   {
      const wfe_binding_t *b = &n->ins[i];
      const wfe_node_t *prod = wfe_def_node(def, b->producer_id);
      if (!prod)
      {
         snprintf(err, errlen, "node '%s': input '%s' references unknown producer '%s'", n->id,
                  b->input_name, b->producer_id);
         return -1;
      }
      wfe_artifact_type_t out = wfe_block_output(prod->block);
      if (!wfe_block_accepts_input(n->block, out))
      {
         snprintf(err, errlen,
                  "node '%s' (%s): input '%s' bound to '%s' which produces %s "
                  "(not an accepted input type)",
                  n->id, wfe_block_name(n->block), b->input_name, b->producer_id,
                  wfe_artifact_name(out));
         return -1;
      }
   }
   return 0;
}

/* gate-specific rules: single-lens floor + optional/pr_review incompatibility. */
static int check_gate_rules(const wfe_node_t *n, char *err, size_t errlen)
{
   if (n->block == WFE_BLK_GATE_ROUNDTABLE)
   {
      const cJSON *panel = n->params ? cJSON_GetObjectItemCaseSensitive(n->params, "panel") : NULL;
      const cJSON *required = panel ? cJSON_GetObjectItemCaseSensitive(panel, "required") : NULL;
      int req = (required && cJSON_IsArray(required)) ? cJSON_GetArraySize(required) : 0;
      if (req < 2)
      {
         snprintf(err, errlen,
                  "node '%s': gate.roundtable requires panel.required >= 2 "
                  "(single-lens gates are forbidden)",
                  n->id);
         return -1;
      }
   }
   if (n->block == WFE_BLK_GATE_HUMAN)
   {
      const char *policy = param_str(n->params, "policy");
      int optional = param_bool(n->params, "optional", 0);
      if (optional && policy && strcmp(policy, "pr_review") == 0)
      {
         snprintf(err, errlen,
                  "node '%s': gate.human optional:true is incompatible with "
                  "policy pr_review",
                  n->id);
         return -1;
      }
   }
   return 0;
}

/* control-edge target must exist (dangling check). */
static int check_edge(const wfe_def_t *def, const wfe_node_t *n, const char *edge,
                      const char *label, char *err, size_t errlen)
{
   if (!edge[0])
      return 0;
   if (!wfe_def_node(def, edge))
   {
      snprintf(err, errlen, "node '%s': %s edge -> unknown node '%s'", n->id, label, edge);
      return -1;
   }
   return 0;
}

static void mark_reachable(const wfe_def_t *def, const char *id, int *seen)
{
   const wfe_node_t *n = wfe_def_node(def, id);
   if (!n)
      return;
   int idx = (int)(n - def->nodes);
   if (seen[idx])
      return;
   seen[idx] = 1;
   if (n->next[0])
      mark_reachable(def, n->next, seen);
   if (n->on_pass[0])
      mark_reachable(def, n->on_pass, seen);
   if (n->on_fail[0])
      mark_reachable(def, n->on_fail, seen);
}

int wfe_def_validate(const wfe_def_t *def, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!def || def->n_nodes == 0)
   {
      snprintf(err, errlen, "empty workflow");
      return -1;
   }

   /* duplicate ids */
   for (int i = 0; i < def->n_nodes; i++)
      for (int j = i + 1; j < def->n_nodes; j++)
         if (strcmp(def->nodes[i].id, def->nodes[j].id) == 0)
         {
            snprintf(err, errlen, "duplicate node id '%s'", def->nodes[i].id);
            return -1;
         }

   /* start must exist */
   if (!wfe_def_node(def, def->start))
   {
      snprintf(err, errlen, "start node '%s' does not exist", def->start);
      return -1;
   }

   /* per-node: block known, inputs typed, gate rules, edges resolve */
   for (int i = 0; i < def->n_nodes; i++)
   {
      const wfe_node_t *n = &def->nodes[i];
      if (n->block == WFE_BLK_UNKNOWN)
      {
         snprintf(err, errlen, "node '%s': unknown block", n->id);
         return -1;
      }
      if (check_inputs(def, n, err, errlen) != 0)
         return -1;
      if (check_gate_rules(n, err, errlen) != 0)
         return -1;
      if (check_edge(def, n, n->next, "next", err, errlen) != 0)
         return -1;
      if (check_edge(def, n, n->on_pass, "on_pass", err, errlen) != 0)
         return -1;
      if (check_edge(def, n, n->on_fail, "on_fail", err, errlen) != 0)
         return -1;
   }

   /* at least one terminal node (no outgoing control edges) */
   int has_terminal = 0;
   for (int i = 0; i < def->n_nodes; i++)
   {
      const wfe_node_t *n = &def->nodes[i];
      if (!n->next[0] && !n->on_pass[0] && !n->on_fail[0])
      {
         has_terminal = 1;
         break;
      }
   }
   if (!has_terminal)
   {
      snprintf(err, errlen, "workflow has no terminal node (graph never ends)");
      return -1;
   }

   /* reachability from start */
   int *seen = calloc((size_t)def->n_nodes, sizeof(int));
   if (!seen)
      return -1;
   mark_reachable(def, def->start, seen);
   for (int i = 0; i < def->n_nodes; i++)
      if (!seen[i])
      {
         snprintf(err, errlen, "node '%s' is unreachable from start '%s'", def->nodes[i].id,
                  def->start);
         free(seen);
         return -1;
      }
   free(seen);
   return 0;
}
