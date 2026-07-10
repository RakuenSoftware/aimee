/* wfe_validate.c -- workflow graph validator: typed-handle resolution + graph
 * health + the single-lens floor. Returns 0 on success; nonzero + err on the
 * first violation found. */
#include "wfe_def.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* display name (the custom block's name for WFE_BLK_CUSTOM, else the catalog). */
static const char *node_block_name(const wfe_node_t *n)
{
   return (n->block == WFE_BLK_CUSTOM) ? n->custom_name : wfe_block_name(n->block);
}

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

/* 1 if `persona` appears in a node's params.panel.required or .eligible. */
static int panel_has_persona(const cJSON *params, const char *persona)
{
   const cJSON *panel = params ? cJSON_GetObjectItemCaseSensitive(params, "panel") : NULL;
   if (!panel || !persona)
      return 0;
   static const char *keys[] = {"required", "eligible"};
   for (int k = 0; k < 2; k++)
   {
      const cJSON *arr = cJSON_GetObjectItemCaseSensitive(panel, keys[k]);
      const cJSON *it = NULL;
      if (arr && cJSON_IsArray(arr))
         cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it) &&
                                         strcmp(it->valuestring, persona) == 0) return 1;
   }
   return 0;
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

   if (wfe_node_requires_input(n) && n->n_ins == 0)
   {
      snprintf(err, errlen, "node '%s' (%s) requires an input binding", n->id, node_block_name(n));
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
      wfe_artifact_type_t out = wfe_node_output(prod);
      if (!wfe_node_accepts_input(n, out))
      {
         snprintf(err, errlen,
                  "node '%s' (%s): input '%s' bound to '%s' which produces %s "
                  "(not an accepted input type)",
                  n->id, node_block_name(n), b->input_name, b->producer_id, wfe_artifact_name(out));
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
      /* A human gate is an inviolable stop: it must not be declared auto-
       * satisfiable. policy:preauthorized and optional:true both asked the
       * autonomy driver to clear the gate without a human — no longer permitted,
       * so reject them at authoring time rather than silently ignoring them. */
      if (policy && strcmp(policy, "preauthorized") == 0)
      {
         snprintf(err, errlen,
                  "node '%s': gate.human policy 'preauthorized' is not allowed — a "
                  "human gate cannot be auto-satisfied in autonomous mode",
                  n->id);
         return -1;
      }
      if (optional)
      {
         snprintf(err, errlen,
                  "node '%s': gate.human optional:true is not allowed — a human gate "
                  "cannot be skipped or auto-satisfied",
                  n->id);
         return -1;
      }
   }
   return 0;
}

/* generic loop-cap params: max_iters (positive int) + on_max (pass/fail/human). */
static int check_loop_cap(const wfe_node_t *n, char *err, size_t errlen)
{
   const cJSON *mi = n->params ? cJSON_GetObjectItemCaseSensitive(n->params, "max_iters") : NULL;
   if (mi && (!cJSON_IsNumber(mi) || mi->valueint <= 0))
   {
      snprintf(err, errlen, "node '%s': max_iters must be a positive integer", n->id);
      return -1;
   }
   const cJSON *om = n->params ? cJSON_GetObjectItemCaseSensitive(n->params, "on_max") : NULL;
   if (om)
   {
      if (!cJSON_IsString(om) ||
          (strcmp(om->valuestring, "pass") != 0 && strcmp(om->valuestring, "fail") != 0 &&
           strcmp(om->valuestring, "human") != 0))
      {
         snprintf(err, errlen, "node '%s': on_max must be one of \"pass\", \"fail\", \"human\"",
                  n->id);
         return -1;
      }
      /* on_max:pass routes forward as if advanced (via on_pass, else next); require
       * a forward edge so the forced-pass path never resolves to an empty target. */
      if (strcmp(om->valuestring, "pass") == 0 && !n->on_pass[0] && !n->next[0])
      {
         snprintf(err, errlen,
                  "node '%s': on_max:pass requires an on_pass or next edge to route forward",
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

   /* node ids must be safe identifiers. They are interpolated into per-node
    * artifact file paths (the manager blocks write <worktree>/.wfe-<id>.json) and
    * into lifecycle stage keys, so a '/' or '..' in an id would allow path
    * traversal out of the worktree. Restrict to [A-Za-z0-9_-]. */
   for (int i = 0; i < def->n_nodes; i++)
   {
      const char *id = def->nodes[i].id;
      if (!id[0])
      {
         snprintf(err, errlen, "empty node id");
         return -1;
      }
      for (const char *p = id; *p; p++)
      {
         char c = *p;
         if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-'))
         {
            snprintf(err, errlen, "node id '%s' has an invalid character (allowed: A-Za-z0-9_-)",
                     id);
            return -1;
         }
      }
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
      if (check_loop_cap(n, err, errlen) != 0)
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

   /* I2 (enforced-workflow delivery gate): in a workflow marked `enforced`,
    * EVERY terminal node (one with no outgoing edge) must be a gate.deliver
    * gate -- not merely at least one. Combined with the "at least one terminal"
    * check above and the "all nodes reachable from start" check below, this
    * guarantees the only way to reach a completed state is by crossing
    * gate.deliver: an alternate exit (e.g. a `merge` terminal) cannot provide a
    * delivery path that skips the gate, and an orphaned gate.deliver is rejected
    * by the reachability check. This is a load-time structural invariant,
    * independent of how the YAML edges happen to be wired, so a misconfigured
    * or hand-edited workflow cannot silently bypass enforcement. */
   if (def->enforced)
   {
      int deliver_terminals = 0;
      for (int i = 0; i < def->n_nodes; i++)
      {
         const wfe_node_t *n = &def->nodes[i];
         if (n->next[0] || n->on_pass[0] || n->on_fail[0])
            continue; /* not a terminal node */
         if (n->block != WFE_BLK_GATE_DELIVER)
         {
            snprintf(err, errlen,
                     "enforced workflow '%s': terminal node '%s' is not a gate.deliver "
                     "(every exit must cross the delivery gate)",
                     def->name, n->id);
            return -1;
         }
         deliver_terminals++;
      }
      if (deliver_terminals == 0)
      {
         snprintf(err, errlen, "enforced workflow '%s' must terminate in a gate.deliver node",
                  def->name);
         return -1;
      }
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

   /* D3 (primary-as-manager, anti-rubber-stamp): a `review` node's explicit
    * `reviewer` persona must be disjoint from every roundtable panel in the
    * workflow -- the primary's pre-roundtable reviewer must not also sit on the
    * panel that then judges the same lane. Enforced at load time so a
    * misconfiguration surfaces at definition time, not mid-run. (Reviewer
    * role-eligibility is a runtime/roster check -- the pure def validator has no
    * agent roster.) */
   for (int i = 0; i < def->n_nodes; i++)
   {
      const wfe_node_t *rv = &def->nodes[i];
      if (rv->block != WFE_BLK_REVIEW)
         continue;
      const char *reviewer = param_str(rv->params, "reviewer");
      if (!reviewer || !reviewer[0])
      {
         /* An enforced workflow's review node MUST name an explicit reviewer
          * persona -- otherwise the disjointness invariant below cannot be
          * checked and the review executor would fall back to an arbitrary
          * delegate that could be a panel member (rubber-stamp bypass by YAML
          * omission). Non-enforced workflows may leave it implicit. */
         if (def->enforced)
         {
            snprintf(err, errlen,
                     "review '%s' in an enforced workflow must set an explicit 'reviewer' "
                     "persona (disjoint from the roundtable panel)",
                     rv->id);
            return -1;
         }
         continue;
      }
      for (int j = 0; j < def->n_nodes; j++)
      {
         if (def->nodes[j].block != WFE_BLK_GATE_ROUNDTABLE)
            continue;
         if (panel_has_persona(def->nodes[j].params, reviewer))
         {
            snprintf(err, errlen,
                     "review '%s' reviewer '%s' also sits on roundtable '%s' panel "
                     "(reviewer must be disjoint from the panel that judges its lane)",
                     rv->id, reviewer, def->nodes[j].id);
            return -1;
         }
      }
   }
   return 0;
}
