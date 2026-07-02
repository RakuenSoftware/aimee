/* wfe_block_resolve.c -- see wfe_block_resolve.h. */
#include "wfe_block_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wfe_binding.h"         /* db1_wfe_binding_get */
#include "wfe_def.h"             /* wfe_def_node, wfe_node_t */
#include "wfe_engine.h"          /* wfe_load_workflow, wfe_def_free */
#include "wfe_externalization.h" /* wfe_is_externalization_tool, _permitted */
#include "wfe_iface.h"           /* wfe_block_type_t */
#include "wfe_store.h"           /* db1_work_item_get */

/* Gate blocks advance via their own satisfaction (roundtable verdict, human
 * decision, CI, delivery) -- NOT via the primary's advance_request. Producing
 * blocks (understand/split/review/implement/document/...) are advanceable. */
static int block_is_gate(wfe_block_type_t b)
{
   switch (b)
   {
   case WFE_BLK_GATE_ROUNDTABLE:
   case WFE_BLK_GATE_HUMAN:
   case WFE_BLK_GATE_CI:
   case WFE_BLK_CHECK_MERGEABLE:
   case WFE_BLK_GATE_DELIVER:
      return 1;
   default:
      return 0;
   }
}

int wfe_block_resolve(const char *session_id, wfe_block_ctx_t *out)
{
   if (out)
      memset(out, 0, sizeof *out);
   if (!session_id || !session_id[0] || !out)
      return 0;

   char wi[80] = "";
   if (db1_wfe_binding_get(session_id, wi, sizeof wi, NULL, 0) != 1 || !wi[0])
      return 0; /* unbound */

   db1_work_item_t item;
   if (db1_work_item_get(wi, &item) != 1)
      return 0; /* vanished work-item -> treat as unbound (no per-block restriction) */

   char err[128] = "";
   wfe_def_t *def = wfe_load_workflow(item.workflow_name, err, sizeof err);
   if (!def)
      return 0;

   int ok = 0;
   const wfe_node_t *node = wfe_def_node(def, item.current_stage);
   if (node)
   {
      out->bound = 1;
      out->enforced = def->enforced;
      snprintf(out->work_item_id, sizeof out->work_item_id, "%s", wi);
      snprintf(out->stage, sizeof out->stage, "%s", item.current_stage);
      out->surface = wfe_block_default_surface(node->block);
      /* gate.deliver passing transitions the run to accepted; before that the run
       * has not earned externalization. */
      out->delivered = (strcmp(item.state, "accepted") == 0);
      out->advanceable = !block_is_gate(node->block);
      ok = 1;
   }
   wfe_def_free(def);
   return ok;
}

wfe_toolcall_action_t wfe_toolcall_decide(wfe_enforce_stage_t stage, int policy_blocks)
{
   if (!policy_blocks)
      return WFE_TC_ALLOW;
   if (wfe_enforce_stage_refuses(stage)) /* hard */
      return WFE_TC_DENY;
   if (wfe_enforce_stage_restricts(stage)) /* soft */
      return WFE_TC_WARN;
   return WFE_TC_ALLOW; /* advisory / off: observe only */
}

wfe_toolcall_action_t wfe_mcp_toolcall_action(const char *session_id, const char *tool_name)
{
   wfe_enforce_stage_t stage = wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE"));
   if (stage == WFE_ENFORCE_OFF || !tool_name || !tool_name[0])
      return WFE_TC_ALLOW;
   /* Only externalization primitives are guarded here; everything else passes (the
    * surface strip handles read/write visibility at ingress). */
   if (!wfe_is_externalization_tool(tool_name))
      return WFE_TC_ALLOW;

   wfe_block_ctx_t ctx;
   if (wfe_block_resolve(session_id, &ctx))
   {
      /* Only ENFORCED runs are externalization-guarded: for them I2 guarantees the
       * terminal is gate.deliver, so delivered==accepted is a sound "gate passed"
       * proxy. A non-enforced bound run is not gated (it made no such promise). */
      if (!ctx.enforced)
         return WFE_TC_ALLOW;
      int policy_blocks = !wfe_externalization_tool_permitted(tool_name, ctx.delivered);
      wfe_toolcall_action_t act = wfe_toolcall_decide(stage, policy_blocks);
      /* Audit the enforcement decision. Templated constant values only (action + dial
       * stage names) -- never the raw tool name or args (echo/injection vector). */
      if (act != WFE_TC_ALLOW)
      {
         char detail[128];
         snprintf(detail, sizeof detail,
                  "{\"guard\":\"externalization\",\"action\":\"%s\",\"stage\":\"%s\"}",
                  act == WFE_TC_DENY ? "deny" : "warn", wfe_enforce_stage_name(stage));
         db1_lifecycle_event_add(ctx.work_item_id, ctx.stage, "toolcall_guard", "enforce-s2",
                                 detail, "", 0);
      }
      return act;
   }

   /* Resolve FAILED. Distinguish a truly UNBOUND session (no binding row -> a generic
    * session, allow) from a BOUND session we could not resolve (vanished work-item /
    * unloadable workflow). Per the Q5 rollout split, an INSTRUMENTATION failure on a
    * KNOWN-bound session must NOT fail open for an externalization primitive under
    * HARD -- it fails CLOSED (a bound session is potentially enforced; we refuse
    * rather than let externalization slip through on a lookup error). */
   char wi[80] = "";
   if (db1_wfe_binding_get(session_id, wi, sizeof wi, NULL, 0) != 1 || !wi[0])
      return WFE_TC_ALLOW; /* truly unbound */
   if (!wfe_enforce_stage_refuses(stage))
      return WFE_TC_ALLOW; /* soft / advisory: observe only */
   db1_lifecycle_event_add(
       wi, "", "toolcall_guard", "enforce-s2",
       "{\"guard\":\"externalization\",\"action\":\"deny\",\"reason\":\"unresolved\"}", "", 0);
   return WFE_TC_DENY;
}
